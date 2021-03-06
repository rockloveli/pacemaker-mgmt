#include "crm/crm.h"
#include "crm/cib.h"
#include "crm/pengine/status.h"
#include "crm/msg_xml.h"
#include "crm/transition.h"

/*
#ifdef SUPPORT_COROSYNC
#undef SUPPORT_COROSYNC
#endif
*/

#ifdef SUPPORT_HEARTBEAT
#undef SUPPORT_HEARTBEAT
#endif

#include <pygui_internal.h>

#include "hbagent.h"
#include "hbagentv2.h"

#if SUPPORT_HEARTBEAT
#include "hb_api.h"
#include "heartbeat.h"
#endif
#include "clplumbing/cl_log.h"
#include "clplumbing/coredumps.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
#include <sys/wait.h>

#include <unistd.h>

#include <errno.h>

#include "LHAResourceTable.h"
#include "LHAResourceStatusUpdate.h"

#if !HAVE_CRM_DATA_T
typedef xmlNode crm_data_t;
#endif

#if !HAVE_PCMK_STRERROR
#  define pcmk_strerror(rc) cib_error2string(rc)
#endif

#ifndef F_CIB_UPDATE_RESULT
#  define F_CIB_UPDATE_RESULT "cib_update_result"
#endif

#ifndef EXECRA_OK
#  define EXECRA_OK 0
#endif

/*
 * Agent MIB value conversion macros from the internal value
 */
/* ref. crm/pengine/complex.h:enum pe_obj_types */
#define PE_OBJ_TYPES2AGENTTYPE(variant) ((int)(variant) + 1)
/* ref. crm/pengine/common.h:enum rsc_role_e */
#define RSC_ROLE_E2AGENTSTATUS(role) ((int)(role)) /* the value is identical */

/* need more than (RID_LEN + sizeof(CRMD_ACTION_xxx) + sizeof(rc_code) */
#define MAX_OP_STR_LEN 256
/* need more than sizeof(rc_code). ref. UNIFORM_RET_EXECRA in lrm/raexec.h */
#define MAX_RCCODE_STR_LEN 4

static cib_t *cib_conn = NULL;
static GPtrArray * gResourceTableV2 = NULL;
static int err_occurs = 0; /* the flag which means an error occurs in callback func. */
extern const char *myid;
extern const char *myuuid;

#if !HAVE_DECL_CRM_CONCAT
static char *
crm_concat(const char *prefix, const char *suffix, char join)
{
    int len = 0;
    char *new_str = NULL;

    CRM_ASSERT(prefix != NULL);
    CRM_ASSERT(suffix != NULL);
    len = strlen(prefix) + strlen(suffix) + 2;

    new_str = calloc(1, (len));
    sprintf(new_str, "%s%c%s", prefix, join, suffix);
    new_str[len - 1] = 0;
    return new_str;
}
#endif

/* for debug */
void debugPrint(crm_data_t *msg, int depth, FILE *fp);

/**
 * Initialize of resource table v2.
 */
int
init_resource_table_v2(void)
{
    
    if (gResourceTableV2) {
        free_resource_table_v2();
    }

    return HA_OK;
}

/**
 * This is the main function to update resource table v2.
 * Return the number of resources.
 */
static int
update_resources_recursively(GListPtr reslist, GListPtr nodelist, int index)
{
    GListPtr gIter1 = NULL;

    if (reslist == NULL) {
        return index;
    }
    /*
     * Set resource info to resource table v2 from data_set,
     * and add it to Glib's array.
     */
    for(gIter1 = reslist; gIter1 != NULL; gIter1 = gIter1->next) {
        resource_t *rsc = gIter1->data;
        GListPtr gIter2 = NULL;

        cl_log(LOG_DEBUG, "resource %s processing.", rsc->id);

        for(gIter2 = nodelist; gIter2 != NULL; gIter2 = gIter2->next) {
            node_t *node = gIter2->data;
            struct hb_rsinfov2 *rsinfo;
            enum rsc_role_e rsstate;

            rsinfo = (struct hb_rsinfov2 *) malloc(sizeof(struct hb_rsinfov2));
            if (!rsinfo) {
                cl_log(LOG_CRIT, "malloc resource info v2 failed.");
                return HA_FAIL;
            }

            rsinfo->resourceid = strdup(rsc->id);
            rsinfo->type = PE_OBJ_TYPES2AGENTTYPE(rsc->variant);

            /* using a temp var to suppress casting warning of the compiler */
            rsstate = rsc->fns->state(rsc, TRUE);
            {
                GListPtr running_on_nodes = NULL;

                rsc->fns->location(rsc, &running_on_nodes, TRUE);
                if (pe_find_node_id(
                    running_on_nodes, node->details->id) == NULL) {
                    /*
                     * if the resource is not running on current node,
                     * its status is "stopped(1)".
                     */
                    rsstate = RSC_ROLE_STOPPED;
                }
               g_list_free(running_on_nodes);
            }
            rsinfo->status = RSC_ROLE_E2AGENTSTATUS(rsstate);
            rsinfo->node = strdup(node->details->uname);

            if (is_not_set(rsc->flags, pe_rsc_managed)) {
                rsinfo->is_managed = LHARESOURCEISMANAGED_UNMANAGED;
            } else {
                rsinfo->is_managed = LHARESOURCEISMANAGED_MANAGED;
            }

            /* get fail-count from <status> */
            {
                char *attr_name = NULL;
                char *attr_value = NULL;
                crm_data_t *tmp_xml = NULL;

                attr_name = crm_concat("fail-count", rsinfo->resourceid, '-');
                attr_value = g_hash_table_lookup(node->details->attrs,
                    attr_name); 
                rsinfo->failcount = crm_parse_int(attr_value, "0");
                free(attr_name);
                free_xml(tmp_xml);
             }

            if (rsc->parent != NULL) {
                rsinfo->parent = strdup(rsc->parent->id);
            } else {
                rsinfo->parent = strdup("");
            }

            /*
             * if the resource stops, and its fail-count is 0,
             * don't list it up.
             */ 
            if (rsinfo->status != LHARESOURCESTATUS_STOPPED || 
                   rsinfo->failcount > 0) {
                rsinfo->index = index++;
                g_ptr_array_add(gResourceTableV2, (gpointer *)rsinfo);
            } else {
                free(rsinfo->resourceid);
                free(rsinfo->node);
                free(rsinfo->parent);
                free(rsinfo);
            }

        } /* end slist_iter(node) */

        /* add resources recursively for group/clone/master */
        index = update_resources_recursively(rsc->children,
            nodelist, index);

    } /* end slist_iter(rsc) */

    return index;
}

/**
 * Send query to the CIB and update the information of resource table v2.
 */
int
update_resource_table_v2(void)
{
    crm_data_t *cib_object = NULL;
    pe_working_set_t data_set;
    int rc = 0;
    int options =  cib_scope_local|cib_sync_call;
    int index;

    if (!gResourceTableV2) {
        cl_log(LOG_ERR, "resource table v2 is NULL.");
        return HA_FAIL;
    }
    free_resource_table_v2();

    rc = cib_conn->cmds->query(cib_conn, NULL, &cib_object, options);
    if (rc != 0) {
        cl_log(LOG_ERR, "CIB query failed: %s", pcmk_strerror(rc));
        return HA_FAIL;
    }
    if (cib_object == NULL) {
        cl_log(LOG_ERR, "CIB query failed: empty result.");
        return HA_FAIL;
    }

    cl_log(LOG_DEBUG, "CIB query done. Updating resource table v2.");

    set_working_set_defaults(&data_set);
    data_set.input = cib_object;
    /* parse cib xml info (cib_object). */
    cluster_status(&data_set);

    index = update_resources_recursively(data_set.resources, data_set.nodes, 1);
    if (index == HA_FAIL) {
        cl_log(LOG_ERR, "Update resources failed.");

        data_set.input = NULL;
        cleanup_calculations(&data_set);
        free_xml(cib_object);
        return HA_FAIL;
    }

    cl_log(LOG_DEBUG, "Updated %d resources.", index-1);


    data_set.input = NULL;
    cleanup_calculations(&data_set);
    free_xml(cib_object);
    return HA_OK;
}

/**
 * Update resource table v2 and return it's pointer.
 * To get latest information of resources.
 * This function is called in LHAResourceTable_load(),
 * only when snmp cache timeout occurs.
 */
GPtrArray *
get_resource_table_v2(void)
{
    update_resource_table_v2();
    return gResourceTableV2;
}

/**
 * Free resource table v2.
 */
void
free_resource_table_v2(void)
{
    struct hb_rsinfov2 *resource;

    if (!gResourceTableV2) {
        return;
    }

    cl_log(LOG_DEBUG, "Freeing %d resources.", gResourceTableV2->len);
    while (gResourceTableV2->len) {
        resource = (struct hb_rsinfov2 *) g_ptr_array_remove_index_fast(gResourceTableV2, 0);
        free(resource->resourceid);
        free(resource->node);
        free(resource->parent);
        free(resource);
    }
    
    return;
}

/**
 * This is the main function to send a trap to tell a resource's status is changed.
 * This function is called when a change occurs on the cib information.
 * First, it parses a received xml message, and if the msg tells that the status of
 * a resource changes, it sends a trap to SNMP manager via Net-SNMP daemon.
 * Note1: It sends a trap when the resouce [stopped|started|been Slave|been Master].
 *        And it sends a trap only when the execution of RA method is succeeded.
 * Note2: If an error occurs in this function, set the variable "err_occurs" to tell
 *        that to the handler. (see: handle_cib_msg())
 */
static void
hbagentv2_update_diff(const char *event, crm_data_t *msg)
{

    /*implement parsing the diff and send a trap */
	/*
    const char *op = NULL;
	*/
    crm_data_t *diff = NULL;
    const char *set_name = NULL;

    crm_data_t *change_set = NULL;
    crm_data_t *lrm_rsc = NULL;
    const char *node_id    = NULL;
    const char *rsc_id    = NULL;
    const char *rsc_op_id    = NULL;
    const char *rc_code = NULL;
    const char *t_magic = NULL;
    const char *operation = NULL;
    char tmp_op_str[MAX_OP_STR_LEN];
    char tmp_rc_str[MAX_RCCODE_STR_LEN];
    char *tmp = tmp_op_str;

#if HAVE_DECL___XML_NEXT
    crm_data_t *node_state = NULL;
#endif

    /* Initialize err flag. */
    err_occurs = 0;

    if (!msg) {
        cl_log(LOG_ERR, "cib message is NULL.");
        err_occurs = 1;
        return;
    }
	/*
    op = cl_get_string(msg, F_CIB_OPERATION);
	*/
    diff = get_message_xml(msg, F_CIB_UPDATE_RESULT);
    if (!diff) {
        cl_log(LOG_ERR, "update result is NULL.");
        return;
    }

    /* for debug */
    /*
    {
        FILE * fp;
        fp = fopen("/tmp/msgdiff.out", "a");
        debugPrint(diff, 0, fp);
        fclose(fp);
    }
    */

    /*
     * start to get the following information from difference of cib xml.
     *   <lrm_rsc_op operation="xxx" rc_code="yyy">
     */

    /* get the head pointer of <status> */
    set_name = "diff-added"; /* we need the cib info only which have been updated. */
    change_set = find_xml_node(diff, set_name, FALSE);
    change_set = find_xml_node(change_set, XML_TAG_CIB, FALSE);
    change_set = find_xml_node(change_set, XML_CIB_TAG_STATUS, FALSE);
    if (!change_set) {
        /* There is no information of <status> */
        free_xml(diff);
        return;
    }

#if !HAVE_DECL___XML_NEXT
    xml_child_iter_filter(
        change_set, node_state, XML_CIB_TAG_STATE,
#else
    for(node_state = __xml_first_child(change_set); node_state != NULL; node_state = __xml_next(node_state)) {
        crm_data_t *lrm_rsc_op = NULL;

    if(STRNCMP_CONST((const char *)node_state->name, XML_CIB_TAG_STATE) != 0) {
        continue;
    }
#endif

        /* get the node id at which the resources changed */
        node_id = crm_element_value(node_state, XML_ATTR_ID);
        if (!node_id) {
            /* There is no information of <node_status> */
            free_xml(diff);
            return;
        }
        if (myuuid != NULL && STRNCMP_CONST(node_id, myuuid) != 0) {
            /* This change is not at my node */
            free_xml(diff);
            return;
        }

        /* get the head pointer of <lrm_resource>  */
        lrm_rsc = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
        lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);
        if (!lrm_rsc) {
            /* There is no information of <lrm_resources> */
            free_xml(diff);
            return; 
        }
        lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCE, FALSE);
        if (!lrm_rsc) {
            /* There is no information of <lrm_resource> */
            free_xml(diff);
            return; 
        }

        /*
         * now, get the head pointer of <lrm_rsc_op>,
         * and parse it's resource id, operation,  and rc_code.
         */
#if !HAVE_DECL___XML_NEXT
        xml_child_iter_filter(
            lrm_rsc, lrm_rsc_op, XML_LRM_TAG_RSC_OP,
#else
        for(lrm_rsc_op = __xml_first_child(lrm_rsc); lrm_rsc_op != NULL; lrm_rsc_op = __xml_next(lrm_rsc_op)) {
        if(STRNCMP_CONST((const char *)lrm_rsc_op->name, XML_LRM_TAG_RSC_OP) != 0) {
            continue;
        }
#endif

            rsc_id = crm_element_value(lrm_rsc, XML_ATTR_ID);
            operation = crm_element_value(lrm_rsc_op, XML_LRM_ATTR_TASK);
            rc_code = crm_element_value(lrm_rsc_op, XML_LRM_ATTR_RC);
            rsc_op_id = crm_element_value(lrm_rsc_op, XML_ATTR_ID);
            t_magic = crm_element_value(lrm_rsc_op, XML_ATTR_TRANSITION_MAGIC);
#if !HAVE_DECL___XML_NEXT
            ); /* end of xml_child_iter_filter(lrm_rsc) */
        ); /* end of xml_child_iter_filter(change_set) */
#else
            }
        }
#endif


    /*
     * Sometimes, the difference of cib infomation doesn't include operation and rc_code.
     * Like when you move resources by hand (ex. crm_resource -M, or -F), or
     * resources move according to <rule> and so on.
     * In these cases, get them from other attributes' value.
     */
    /* get operation from lrm_rsc_op's id */
    if (!operation) {
        strcpy(tmp_op_str, rsc_op_id);
        tmp = strrchr(tmp_op_str, '_');
        *tmp = '\0';    
        operation = strrchr(tmp_op_str, '_') + 1;
    }
    /* get rc_code from transition magic number */
    if (!rc_code) {
        if (t_magic != NULL) {
            int transition_id = -1;
            int action_id = -1;
            int status = -1;
            int op_rc = -1;
            int target_rc = -1;
            char *uuid = NULL;

            if (!decode_transition_magic(
                    t_magic, &uuid, &transition_id, &action_id,
                    &status, &op_rc, &target_rc)) {
                cl_log(LOG_ERR, "decode_transition_magic() is failed.");
                free_xml(diff);
                return;
            }
            free(uuid);
            sprintf(tmp_rc_str, "%d\n", op_rc);
            rc_code = tmp_rc_str;
        }
    }

    /* now, send a trap. */
    if (operation != NULL && rc_code != NULL) {
        if (atoi(rc_code) == EXECRA_OK) {
            struct hb_rsinfov2 resource;

            resource.resourceid = strdup(rsc_id);
#if SUPPORT_HEARTBEAT
            if (is_heartbeat_cluster()) {
                resource.node = strdup(myid);
            } else {
                resource.node = strdup(node_id);
            }
#else
            resource.node = strdup(node_id);
#endif

            /* LHAResourceStatus is ... */
            if (safe_str_eq(operation, CRMD_ACTION_STOP)) {
                /* 1: Stopped */
                resource.status = LHARESOURCESTATUS_STOPPED;
            } else if (safe_str_eq(operation, CRMD_ACTION_START)) {
                /* 2: Started */
                resource.status = LHARESOURCESTATUS_STARTED;
            } else if (safe_str_eq(operation, CRMD_ACTION_DEMOTE)) {
                /* 3: Slave */
                resource.status = LHARESOURCESTATUS_SLAVE;
            } else if (safe_str_eq(operation, CRMD_ACTION_PROMOTE)) {
                /* 4: Master */
                resource.status = LHARESOURCESTATUS_MASTER;
            } else {
                /* other action. send no trap. */
                free(resource.resourceid);
                free(resource.node);
                free_xml(diff);
                return;
            }
    
            send_LHAResourceStatusUpdate_trap(&resource);
            free(resource.resourceid);
            free(resource.node);
        } else {
            /* operation does not succeed.  */
            /* do nothing (for the present) */
        }
    }

    free_xml(diff);
    return;    

}

/**
 * Initialization of connection to the CIB.
 * Have a connection made newly. And set callback function, hbagentv2_update_diff(),
 * that is called when cib infomation message changes.
 */
static int
init_cib(int *cib_fd)
{
    int rc;

    
    cib_conn = cib_new();
    if (cib_conn == NULL) {
        cl_log(LOG_ERR, "CIB connection initialization failed.");
        return HA_FAIL;
    }
#if USE_LIBQB_IPC
    rc = cib_conn->cmds->signon_raw(cib_conn, LHAAGENTID, cib_query, cib_fd);
#else
    rc = cib_conn->cmds->signon(cib_conn, LHAAGENTID, cib_query);
#endif
    if (rc != 0) {
        cl_log(LOG_ERR, "CIB connection signon failed.");
        return HA_FAIL;
    }

    rc = cib_conn->cmds->add_notify_callback(cib_conn, T_CIB_DIFF_NOTIFY,
                                             hbagentv2_update_diff);
    if (rc != 0) {
        cl_log(LOG_ERR, "CIB connection adding callback failed.");
        return HA_FAIL;
    }

    cl_log(LOG_INFO, "CIB connection initialization completed.");
    return HA_OK;
}

/**
 * Sign off and clean the connection to the CIB.
 */
static void
free_cib(void)
{
    if (cib_conn) {
        if (cib_conn->cmds->signoff(cib_conn) != 0) {
            cl_log(LOG_WARNING, "CIB connection signoff failed(ignored).");
        }
        cib_delete(cib_conn);
        cib_conn = NULL;
    }

    return;
}

/**
 * Returns a file discripter of the connection to the CIB.
 *   -1   : error occurs.
 *   else : cib conn's fd.
 */
int
get_cib_fd(void)
{
    int fd;
    if (!cib_conn) {
        cl_log(LOG_ERR, "CIB is not initialized.");
        return -1;
    }

    if (cib_conn->cmds->inputfd == NULL
        || (fd = cib_conn->cmds->inputfd(cib_conn)) < 0) {
        cl_log(LOG_ERR, "Can not get CIB inputfd.");
        return -1;
    }
    return fd;
}

/**
 * Handler of cib information message changes.
 * Set this function in the select loop to send a trap.
 */
/* TODO: this prototype is not exported in any headers */
/* Beekhof: Uh yeah, for a _reason_ */

#if !HAVE_CRM_IPC_NEW
gboolean cib_native_dispatch(IPC_Channel *channel, gpointer user_data);
IPC_Channel *cib_native_channel(cib_t* cib);
#else
bool cib_native_dispatch(cib_t * cib);
#endif

int
handle_cib_msg(void)
{
    /* call callback function. */
#if !HAVE_CRM_IPC_NEW
    if (!cib_native_dispatch(cib_native_channel(cib_conn), cib_conn)) {
#else
    if (!cib_native_dispatch(cib_conn)) {
#endif
        cl_log(LOG_ERR, "cib_native_dispatch() failed.");
        return HA_FAIL;
    }
    
    /* check if an error occurs in callback function. */
    if (err_occurs) {
        return HA_FAIL;
    }
    return HA_OK;
}

/**
 * Initialization of hbagentv2.
 *   hbagentv2 is agent which keeps watching on operation of resources.
 *   (see: LHAResourceTable and LHAResourceStatusUpdate)
 */
int
init_hbagentv2(int *cib_fd)
{
    int ret;

    gResourceTableV2 = g_ptr_array_new();
    if (gResourceTableV2 == NULL) {
        cl_log(LOG_ERR, "Storage allocation failure for hbagentv2.");
        return HA_FAIL;
    }

    ret = init_cib(cib_fd);
    if (ret != HA_OK) {
        cl_log(LOG_ERR, "init_cib() failed.");
        return ret;
    }
    ret = init_resource_table_v2();
    if (ret != HA_OK) {
        cl_log(LOG_ERR, "resource table v2 initialization failure.");    
    }

    init_LHAResourceTable();

    cl_log(LOG_INFO, "hbagentv2 initialization completed.");

    return HA_OK;
}

/**
 * Free and clear resource table v2 and cib connection.
 * Disposer which is called before stopping agent.
 */
void
free_hbagentv2(void)
{
    free_resource_table_v2();
    g_ptr_array_free(gResourceTableV2, 1);
    gResourceTableV2 = NULL;
    free_cib();
}

/*
 * debug print for cib info.
 */
void
debugPrint(crm_data_t *msg, int depth, FILE *fp)
{
#if 0
    /* non functional for crm_data_t == xmlNode */
    int i;

    if (msg == NULL) {
        return;
    }

    if (fp == NULL) {
        fp = stdout;
    }

    for (i = 0; i < msg->nfields; i++) {
        int j;
        for (j = 0; j < depth; j++) {
            fprintf(fp, "    ");
            fflush(fp);
        }
        if (msg->types[i] == FT_STRING) {
            fprintf(fp, "[%2d] %s = %s\n", i, msg->names[i], (char*)msg->values[i]);
            fflush(fp);
        }
        else if (msg->types[i] == FT_STRUCT || msg->types[i] == FT_UNCOMPRESS) {
            fprintf(fp, "[%2d] %s {\n", i, msg->names[i]);
            fflush(fp);
            debugPrint(msg->values[i], depth + 1, fp);

            for (j = 0; j < depth; j++) {
                fprintf(fp, "    ");
                fflush(fp);
            }
            fprintf(fp, "}\n");
            fflush(fp);
        }
        else {
            fprintf(fp, "[%2d] %s is [%d] TYPE.\n", i, msg->names[i], msg->types[i]);
            fflush(fp);
        }
    }
#endif
}

/**
 * Emacs stuff:
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
