#include <stdint.h>
#include <string.h>

/* HAL */
#include "boards.h"
#include "nrf_delay.h"
#include "simple_hal.h"

/* Core */
#include "nrf_mesh.h"
#include "nrf_mesh_events.h"
#include "nrf_mesh_assert.h"
#include "access_config.h"
#include "device_state_manager.h"
#include "flash_manager.h"
#include "mesh_stack.h"
#include "net_state.h"

/* Provisioning and configuration */
#include "provisioner_helper.h"
#include "node_setup.h"
#include "mesh_app_utils.h"
#include "mesh_softdevice_init.h"

/* Models */
#include "config_client.h"
#include "config_server.h"
#include "health_client.h"
#include "simple_smart_city_common.h"

/* Logging and RTT */
#include "rtt_input.h"
#include "log.h"

/* Example specific includes */
#include "simple_smart_city_example_common.h"
#include "example_network_config.h"
#include "nrf_mesh_config_examples.h"
#include "app_timer.h"

#define APP_NETWORK_STATE_ENTRY_HANDLE (0x0001)
#define APP_FLASH_PAGE_COUNT           (1)

#define PROV_START_DELAY APP_TIMER_TICKS(5000) // O provisionamento iniciará após cinco segundos

APP_TIMER_DEF(m_timer_id);

/* Required for the provisioner helper module */
static network_dsm_handles_data_volatile_t m_dev_handles;
static network_stats_data_stored_t m_nw_state;

static const uint8_t m_device_uuid_filter[SMART_CITY_NODE_UUID_PREFIX_SIZE] = SMART_CITY_NODE_UUID_PREFIX;
static prov_helper_uuid_filter_t m_exp_uuid;
static bool m_node_prov_setup_started;

/* Forward declarations */
static void app_health_event_cb(const health_client_t * p_client, const health_client_evt_t * p_event);
static void app_config_successful_cb(void);
static void app_config_failed_cb(void);
static void app_mesh_core_event_cb (const nrf_mesh_evt_t * p_evt);

static void app_start(void);

static nrf_mesh_evt_handler_t m_mesh_core_event_handler = { .evt_cb = app_mesh_core_event_cb };

/*****************************************************************************/
/**** Flash handling ****/
#if PERSISTENT_STORAGE

static flash_manager_t m_flash_manager;

static void app_flash_manager_add(void);

static void flash_write_complete(const flash_manager_t * p_manager, const fm_entry_t * p_entry, fm_result_t result)
{
     __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Flash write complete\n");

    /* If we get an AREA_FULL then our calculations for flash space required are buggy. */
    NRF_MESH_ASSERT(result != FM_RESULT_ERROR_AREA_FULL);

    /* We do not invalidate in this module, so a NOT_FOUND should not be received. */
    NRF_MESH_ASSERT(result != FM_RESULT_ERROR_NOT_FOUND);
    if (result == FM_RESULT_ERROR_FLASH_MALFUNCTION)
    {
        ERROR_CHECK(NRF_ERROR_NO_MEM);
    }
}

static void flash_invalidate_complete(const flash_manager_t * p_manager, fm_handle_t handle, fm_result_t result)
{
    /* This application does not expect invalidate complete calls. */
    ERROR_CHECK(NRF_ERROR_INTERNAL);
}

typedef void (*flash_op_func_t) (void);
static void flash_manager_mem_available(void * p_args)
{
    ((flash_op_func_t) p_args)(); /*lint !e611 Suspicious cast */
}


static void flash_remove_complete(const flash_manager_t * p_manager)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Flash remove complete\n");
}

static void app_flash_manager_add(void)
{

    static fm_mem_listener_t flash_add_mem_available_struct = {
        .callback = flash_manager_mem_available,
        .p_args = app_flash_manager_add
    };
    flash_manager_config_t manager_config;
    manager_config.write_complete_cb = flash_write_complete;
    manager_config.invalidate_complete_cb = flash_invalidate_complete;
    manager_config.remove_complete_cb = flash_remove_complete;
    manager_config.min_available_space = WORD_SIZE;
    manager_config.p_area = (const flash_manager_page_t *) (((const uint8_t *) dsm_flash_area_get()) - (ACCESS_FLASH_PAGE_COUNT * PAGE_SIZE * 2));
    manager_config.page_count = APP_FLASH_PAGE_COUNT;
    uint32_t status = flash_manager_add(&m_flash_manager, &manager_config);
    if (NRF_SUCCESS != status)
    {
        flash_manager_mem_listener_register(&flash_add_mem_available_struct);
        __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Unable to add flash manager for app data\n");
    }
}

static bool load_app_data(void)
{
    flash_manager_wait();
    const fm_entry_t * p_entry = flash_manager_entry_get(&m_flash_manager, APP_NETWORK_STATE_ENTRY_HANDLE);
    if (p_entry == NULL)
    {
        memset(&m_nw_state, 0x00, sizeof(m_nw_state));
        return false;
    }

    memcpy(&m_nw_state, p_entry->data, sizeof(m_nw_state));
    return true;
}

static uint32_t store_app_data(void)
{
    fm_entry_t * p_entry = flash_manager_entry_alloc(&m_flash_manager, APP_NETWORK_STATE_ENTRY_HANDLE, sizeof(m_nw_state));
    static fm_mem_listener_t flash_add_mem_available_struct = {
        .callback = flash_manager_mem_available,
        .p_args = store_app_data
    };

    if (p_entry == NULL)
    {
        flash_manager_mem_listener_register(&flash_add_mem_available_struct);
    }
    else
    {
        network_stats_data_stored_t * p_nw_state = (network_stats_data_stored_t *) p_entry->data;
        memcpy(p_nw_state, &m_nw_state, sizeof(m_nw_state));
        flash_manager_entry_commit(p_entry);
    }

    return NRF_SUCCESS;
}

static void clear_app_data(void)
{
    memset(&m_nw_state, 0x00, sizeof(m_nw_state));

    if (flash_manager_remove(&m_flash_manager) != NRF_SUCCESS)
    {
        /* Register the listener and wait for some memory to be freed up before we retry. */
        static fm_mem_listener_t mem_listener = {.callback = flash_manager_mem_available,
                                                 .p_args = clear_app_data};
        flash_manager_mem_listener_register(&mem_listener);
    }
}

#else

static void clear_app_data(void)
{
    return;
}

bool load_app_data(void)
{
    return false;
}
static uint32_t store_app_data(void)
{
    return NRF_SUCCESS;
}

#endif


static void app_data_store_cb(void)
{
    ERROR_CHECK(store_app_data());
}

/*****************************************************************************/
/**** Configuration process related callbacks ****/

static void app_config_successful_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Configuration of device %u successful\n", m_nw_state.configured_devices);

    m_nw_state.configured_devices++;
    access_flash_config_store();
    ERROR_CHECK(store_app_data());

    if (m_nw_state.configured_devices < SMART_CITY_DEVICE_COUNT)
    {
        m_exp_uuid.p_uuid = m_device_uuid_filter;
        m_exp_uuid.length = SMART_CITY_NODE_UUID_PREFIX_SIZE;
        prov_helper_provision_next_device(PROVISIONER_RETRY_COUNT, m_nw_state.next_device_address, &m_exp_uuid);
        prov_helper_scan_start();
    }
    else
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "All servers provisioned\n");
    }
}

static void prov_retry(void)
{
    uint32_t err_code;
    err_code = app_timer_start(m_timer_id,PROV_START_DELAY,NULL);
    APP_ERROR_CHECK(err_code);
}

static void app_config_failed_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Configuration of device %u failed. Retrying in five seconds \n", m_nw_state.configured_devices);
    prov_retry();
    m_node_prov_setup_started = false;
}

static void app_prov_success_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisioning successful\n");
}

static void app_prov_failed_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisioning failed. Retrying in five seconds \n");
    prov_retry();
    m_node_prov_setup_started = false;
}


/*****************************************************************************/
/**** Model related callbacks ****/
static void app_health_event_cb(const health_client_t * p_client, const health_client_evt_t * p_event)
{
    switch (p_event->type)
    {
        case HEALTH_CLIENT_EVT_TYPE_CURRENT_STATUS_RECEIVED:
            __LOG(LOG_SRC_APP,
                  LOG_LEVEL_INFO,
                  "Node 0x%04x alive with %u active fault(s), RSSI: %d\n",
                  p_event->p_meta_data->src.value,
                  p_event->data.fault_status.fault_array_length,
                  ((p_event->p_meta_data->p_core_metadata->source == NRF_MESH_RX_SOURCE_SCANNER)
                       ? p_event->p_meta_data->p_core_metadata->params.scanner.rssi
                       : 0));
            break;
        default:
            break;
    }
}

static void app_config_server_event_cb(const config_server_evt_t * p_evt)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "config_server Event %d.\n", p_evt->type);

    if (p_evt->type == CONFIG_SERVER_EVT_NODE_RESET)
    {
        /* This should never return */
        hal_device_reset(0);
    }
}

static void app_config_client_event_cb(config_client_event_type_t event_type, const config_client_event_t * p_event, uint16_t length)
{
    /* USER_NOTE: Do additional processing of config client events here if required */
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Config client event\n");

    /* Pass events to the node setup helper module for further processing */
    node_setup_config_client_event_process(event_type, p_event, length);
}


/** Check if all devices have been provisioned. If not, provision remaining devices.
 *  Check if all devices have been configured. If not, start configuring them.
 */
static void check_network_state(void)
{
    if (!m_node_prov_setup_started)
    {
        /* If previously provisioned device is not configured, start node setup procedure. */
        if (m_nw_state.configured_devices < m_nw_state.provisioned_devices)
        {
            /* Execute configuration */
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Waiting for provisioned node to be configured ...\n");
            node_setup_start(m_nw_state.last_device_address, PROVISIONER_RETRY_COUNT,
                            m_nw_state.appkey, APPKEY_INDEX);
        }
        else if (m_nw_state.provisioned_devices < SMART_CITY_DEVICE_COUNT)
        {
            /* Start provisioning - rest of the devices */
            m_exp_uuid.p_uuid = m_device_uuid_filter;
            m_exp_uuid.length = SMART_CITY_NODE_UUID_PREFIX_SIZE;
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Waiting for device to be provisioned ...\n");
            prov_helper_provision_next_device(PROVISIONER_RETRY_COUNT, m_nw_state.next_device_address, &m_exp_uuid);
            prov_helper_scan_start();
        }
        m_node_prov_setup_started = true;
    }
    else
    {
         __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Waiting for previous procedure to finish ...\n");
    }
}

static void app_mesh_core_event_cb (const nrf_mesh_evt_t * p_evt)
{
    /* USER_NOTE: User can insert mesh core event proceesing here */
    switch(p_evt->type)
    {
        /* Start user application specific functions only when flash is stable */
        case NRF_MESH_EVT_FLASH_STABLE:
            __LOG(LOG_SRC_APP, LOG_LEVEL_DBG1, "Mesh evt: FLASH_STABLE \n");
#if (PERSISTENT_STORAGE)
            {
                static bool s_app_started;
                if (!s_app_started)
                {
                    /* Flash operation initiated during initialization has been completed */
                    app_start();
                    s_app_started = true;
                }
            }
#endif
            break;

        default:
            __LOG(LOG_SRC_APP, LOG_LEVEL_DBG1, "Unhandled Mesh Event: %d \n", p_evt->type);
            break;
    }
}

/* Binds the local models correctly with the desired keys */
void app_default_models_bind_setup(void)
{
    /* Bind health client to App key, and configure publication key */
    ERROR_CHECK(access_model_application_bind(m_dev_handles.m_health_client_instance.model_handle, m_dev_handles.m_appkey_handle));
    ERROR_CHECK(access_model_publish_application_set(m_dev_handles.m_health_client_instance.model_handle, m_dev_handles.m_appkey_handle));

    /* Bind self-config server to the self device key */
    ERROR_CHECK(config_server_bind(m_dev_handles.m_self_devkey_handle));
}


static bool app_flash_config_load(void)
{
    bool app_load = false;
#if PERSISTENT_STORAGE
    app_flash_manager_add();
    app_load = load_app_data();
#endif
    if (!app_load)
    {
        m_nw_state.provisioned_devices = 0;
        m_nw_state.configured_devices = 0;
        m_nw_state.next_device_address = UNPROV_START_ADDRESS;
        ERROR_CHECK(store_app_data());
    }
    else
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Restored: App data\n");
    }
    return app_load;
}

static void timer_handler(void * p_context)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisionamento iniciado\n");
    check_network_state();
}

static void timer_init(void)
{
    ret_code_t err_code;
    // inicializa o temporizador
    err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);
    // cria o temporizador
    err_code = app_timer_create(&m_timer_id,APP_TIMER_MODE_SINGLE_SHOT,timer_handler);
    APP_ERROR_CHECK(err_code);
}

void models_init_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Initializing and adding models\n");
    m_dev_handles.m_netkey_handle = DSM_HANDLE_INVALID;
    m_dev_handles.m_appkey_handle = DSM_HANDLE_INVALID;
    m_dev_handles.m_self_devkey_handle = DSM_HANDLE_INVALID;

    /* This app requires following models :
     * config client : To be able to configure other devices
     * health client : To be able to interact with other health servers */
    ERROR_CHECK(config_client_init(app_config_client_event_cb));
    ERROR_CHECK(health_client_init(&m_dev_handles.m_health_client_instance, 0, app_health_event_cb));
}

static void mesh_init(void)
{
    bool device_provisioned;
    mesh_stack_init_params_t init_params =
    {
        .core.irq_priority       = NRF_MESH_IRQ_PRIORITY_LOWEST,
        .core.lfclksrc           = DEV_BOARD_LF_CLK_CFG,
        .models.models_init_cb   = models_init_cb,
        .models.config_server_cb = app_config_server_event_cb
    };
    ERROR_CHECK(mesh_stack_init(&init_params, &device_provisioned));

    nrf_mesh_evt_handler_add(&m_mesh_core_event_handler);

    /* Load application configuration, if available */
    m_dev_handles.flash_load_success = app_flash_config_load();

    /* Initialize the provisioner */
    mesh_provisioner_init_params_t m_prov_helper_init_info =
    {
        .p_dev_data = &m_dev_handles,
        .p_nw_data = &m_nw_state,
        .netkey_idx = NETKEY_INDEX,
        .p_data_store_cb  = app_data_store_cb,
        .p_prov_success_cb = app_prov_success_cb,
        .p_prov_failed_cb = app_prov_failed_cb
    };
    prov_helper_init(&m_prov_helper_init_info);

    if (!device_provisioned)
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Setup defaults: Adding keys, addresses, and bindings \n");

        prov_helper_provision_self();
        app_default_models_bind_setup();
        access_flash_config_store();
        app_data_store_cb();
    }
    else
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Restored: Handles \n");
        prov_helper_device_handles_load();
    }

    node_setup_cb_set(app_config_successful_cb, app_config_failed_cb);
}

static void initialize(void)
{
    __LOG_INIT(LOG_SRC_APP | LOG_SRC_ACCESS, LOG_LEVEL_INFO, LOG_CALLBACK_DEFAULT);
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "----- BLE Mesh Light Switch Provisioner Demo -----\n");

    /* Mesh Init */
    nrf_clock_lf_cfg_t lfc_cfg = DEV_BOARD_LF_CLK_CFG;
    ERROR_CHECK(mesh_softdevice_init(lfc_cfg));
    mesh_init();
}

static void app_start(void)
{
    m_nw_state.next_device_address=((m_nw_state.self_devkey[1]<<8)+m_nw_state.self_devkey[0])&0x7fff;
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Starting application ...\n");
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisoned Nodes: %d, Configured Nodes: %d Next Address: 0x%04x\n",
          m_nw_state.provisioned_devices, m_nw_state.configured_devices, m_nw_state.next_device_address);
    __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "Dev key ", m_nw_state.self_devkey, NRF_MESH_KEY_SIZE);
    __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "Net key ", m_nw_state.netkey, NRF_MESH_KEY_SIZE);
    __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "App key ", m_nw_state.appkey, NRF_MESH_KEY_SIZE);
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisioning will start in five seconds\n");
    prov_retry();
}

static void start(void)
{
    ERROR_CHECK(nrf_mesh_enable());
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "<start> \n");

#if (!PERSISTENT_STORAGE)
    app_start();
#endif
}

int main(void)
{
    initialize();
    timer_init();
    execution_start(start);

    for (;;)
    {
        (void)sd_app_evt_wait();
    }
}
