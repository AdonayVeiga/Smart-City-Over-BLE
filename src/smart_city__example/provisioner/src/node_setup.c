#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>

#include "nrf_mesh_utils.h"
#include "device_state_manager.h"
#include "timer_scheduler.h"

#include "config_client.h"
#include "config_server.h"
#include "access_config.h"
#include "smart_city_semaforo_full.h"
#include "health_common.h"
#include "composition_data.h"

#include "node_setup.h"
#include "example_network_config.h"
#include "simple_smart_city_example_common.h"
#include "mesh_app_utils.h"

#include "log.h"

// Pela premissa, o código do modelo deve ser um endereço de grupo multicast
#define IS_SMART_CITY_MODEL(model)(model >= 0xC000)

/* USER_NOTE: Add more steps here is you want to customize the nodes further. */
/* Node setup steps */
typedef enum
{
    NODE_SETUP_IDLE,
    NODE_SETUP_CONFIG_COMPOSITION_GET,
    NODE_SETUP_CONFIG_APPKEY_ADD,
    NODE_SETUP_CONFIG_APPKEY_BIND_HEALTH,
    NODE_SETUP_CONFIG_APPKEY_BIND_SERVICE,
    NODE_SETUP_CONFIG_PUBLICATION_HEALTH,
    NODE_SETUP_CONFIG_PUBLICATION_SERVICE,
    NODE_SETUP_CONFIG_SUBSCRIPTION_SERVICE,
    NODE_SETUP_DONE,
} config_steps_t;

/* Structure to hold the composition data */
typedef struct
{
    uint16_t    len;
    struct
    {
        uint8_t page_number;
        uint8_t data[NRF_MESH_SEG_PAYLOAD_SIZE_MAX];
    }composition;
} composition_data_t;

/* Expected status structure, used for setup state machine */
typedef struct
{
    uint8_t  num_statuses;
    uint16_t expected_opcode;
    const uint8_t  * p_statuses;
} expected_status_list_t;

typedef struct
{
    uint8_t count;
    timer_event_t timer;
} client_send_retry_t;

typedef enum
{
    STATUS_CHECK_PASS,
    STATUS_CHECK_FAIL,
    STATUS_CHECK_UNEXPECTED_OPCODE
} status_check_t;

/* USER_NOTE:
You can define one or more such configuration steps for a given node in your network. The choice
of the steps can be done in @ref setup_select_steps() function.
*/
/* Sequência de passos para configurar os dispositivos */
static const config_steps_t smart_city_device_config_steps[] =
{
    NODE_SETUP_CONFIG_COMPOSITION_GET,
    NODE_SETUP_CONFIG_APPKEY_ADD,
    NODE_SETUP_CONFIG_APPKEY_BIND_HEALTH,
    NODE_SETUP_CONFIG_PUBLICATION_HEALTH,
    NODE_SETUP_DONE
};

// Sequência de passos para configurar os modelos da cidade inteligente
static const config_steps_t smart_city_models_config_steps[]=
{
    NODE_SETUP_CONFIG_APPKEY_BIND_SERVICE,
    NODE_SETUP_CONFIG_PUBLICATION_SERVICE,
    NODE_SETUP_CONFIG_SUBSCRIPTION_SERVICE,
    NODE_SETUP_DONE
};

static uint16_t m_current_node_addr;
static composition_data_t m_node_composition;
static uint16_t m_retry_count;
static client_send_retry_t m_send_timer;
static const uint8_t * mp_appkey;
static uint16_t m_appkey_idx;
static uint8_t current_model_count;
static access_model_id_t* current_model_id=NULL;
static config_composition_element_header_t * element_header=NULL;

static const config_steps_t m_idle_step = NODE_SETUP_IDLE;
static const config_steps_t * mp_config_step = &m_idle_step;
static node_setup_successful_cb_t m_node_setup_success_cb;
static node_setup_failed_cb_t m_node_setup_failed_cb;
static expected_status_list_t m_expected_status_list;
static bool m_status_checked;

/* Forward declaration */
static void config_step_execute(void);

/*************************************************************************************************/
/* Set expected status opcode and acceptable value of status codes */
static void expected_status_set(uint32_t opcode, uint32_t n, const uint8_t * p_list)
{
    if (n > 0)
    {
        NRF_MESH_ASSERT(p_list != NULL);
    }

    m_expected_status_list.expected_opcode = opcode;
    m_expected_status_list.num_statuses = n;
    m_expected_status_list.p_statuses = p_list;
    m_status_checked = false;
}

/* setup retry timer, if client model is busy sending a previous message */
static void retry_on_fail(uint32_t ret_status)
{
    if (ret_status == NRF_ERROR_BUSY && m_send_timer.count)
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Client busy, retrying ...\n");
        config_client_pending_msg_cancel();
        m_send_timer.timer.timestamp = timer_now() + MS_TO_US(CLIENT_BUSY_SEND_RETRY_DELAY_MS);
        timer_sch_schedule(&m_send_timer.timer);
        m_send_timer.count--;
    }
    else
    {
        ERROR_CHECK(ret_status);
    }
}

/* Callback for the timer event */
static void client_send_timer_cb(timestamp_t timestamp, void * p_context)
{
    timer_sch_abort(&m_send_timer.timer);

    /* retry the last step */
    config_step_execute();
}

/**
 *  When config client status message is received, this function checks for the expected opcode, and
 *  status values. It is required by the node setup state machine.
 */
static status_check_t check_expected_status(uint16_t rx_opcode, const config_msg_t * p_msg)
{
    uint8_t status = 0xFF;
    if (rx_opcode != m_expected_status_list.expected_opcode)
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Unexpected opcode: exp 0x%04x  rx 0x%04x\n",
              m_expected_status_list.expected_opcode, rx_opcode);
        return STATUS_CHECK_UNEXPECTED_OPCODE;
    }

    m_status_checked = true;
    switch (rx_opcode)
    {
        /* COMPOSITION_DATA_STATUS does not have a STATUS field */
        case CONFIG_OPCODE_COMPOSITION_DATA_STATUS:
            break;

        case CONFIG_OPCODE_MODEL_APP_STATUS:
            status = p_msg->app_status.status;
            break;

        case CONFIG_OPCODE_MODEL_PUBLICATION_STATUS:
            status = p_msg->publication_status.status;
            break;

        case CONFIG_OPCODE_MODEL_SUBSCRIPTION_STATUS:
            status = p_msg->subscription_status.status;
            break;

        case CONFIG_OPCODE_APPKEY_STATUS:
            status = p_msg->appkey_status.status;
            break;

        default:
            /** USER_TO_CONFIGURE: Resolve additional required statuses in above switch case */
            __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Handle additional statuses here");
            ERROR_CHECK(NRF_ERROR_NOT_FOUND);
            break;
    }

    if (m_expected_status_list.num_statuses == 0)
    {
        return STATUS_CHECK_PASS;
    }
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "opcode status field: %d \n", status);
    for(uint32_t i = 0; i<m_expected_status_list.num_statuses; i++)
    {
        if (status == m_expected_status_list.p_statuses[i])
        {
            return STATUS_CHECK_PASS;
        }
    }
    return STATUS_CHECK_FAIL;
}

/*************************************************************************************************/
/* Node setup functionality related static functions */
/* USER_NOTE:
You can code any suitable logic here to select the configuration steps for a given
node in your network.
*/
/**
 * Selects the configuration steps for the node.
 * @param[in]  addr    Address of the device being configured.
 *
 */
static void setup_select_steps(uint16_t addr)
{
    mp_config_step = smart_city_device_config_steps;
}


/** Step execution function for the configuration state machine. */
static void config_step_execute(void)
{
    uint32_t status;
    switch (*mp_config_step)
    {
        /* Read the composition data from the node: */
        case NODE_SETUP_CONFIG_COMPOSITION_GET:
        {
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Getting composition data\n");
            retry_on_fail(config_client_composition_data_get(0x00));

            expected_status_set(CONFIG_OPCODE_COMPOSITION_DATA_STATUS, 0, NULL);
            break;
        }

        /* Add the application key to the node: */
        case NODE_SETUP_CONFIG_APPKEY_ADD:
        {
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Adding appkey\n");
            retry_on_fail(config_client_appkey_add(NETKEY_INDEX, m_appkey_idx, mp_appkey));

            static const uint8_t exp_status[] = {ACCESS_STATUS_SUCCESS, ACCESS_STATUS_KEY_INDEX_ALREADY_STORED};
            expected_status_set(CONFIG_OPCODE_APPKEY_STATUS, sizeof(exp_status), exp_status);
            break;
        }

        /* Bind the health server to the application key: */
        case NODE_SETUP_CONFIG_APPKEY_BIND_HEALTH:
        {
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "App key bind: Health server\n");
            access_model_id_t model_id;
            model_id.company_id = ACCESS_COMPANY_ID_NONE;
            model_id.model_id = HEALTH_SERVER_MODEL_ID;
            uint16_t element_address = m_current_node_addr;
            retry_on_fail(config_client_model_app_bind(element_address, m_appkey_idx, model_id));

            static const uint8_t exp_status[] = {ACCESS_STATUS_SUCCESS};
            expected_status_set(CONFIG_OPCODE_MODEL_APP_STATUS, sizeof(exp_status), exp_status);
            break;
        }

        /* Bind the semaforo service to the application key: */
        case NODE_SETUP_CONFIG_APPKEY_BIND_SERVICE:
        {
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "App key bind: Smart City Service\n");
            access_model_id_t model_id;
            model_id.company_id = current_model_id->company_id;
            model_id.model_id = current_model_id->model_id;
            uint16_t element_address = m_current_node_addr;
            retry_on_fail(config_client_model_app_bind(element_address, m_appkey_idx, model_id));

            static const uint8_t exp_status[] = {ACCESS_STATUS_SUCCESS};
            expected_status_set(CONFIG_OPCODE_MODEL_APP_STATUS, sizeof(exp_status), exp_status);
            break;
        }

        /* Configure the publication parameters for the Health server: */
        case NODE_SETUP_CONFIG_PUBLICATION_HEALTH:
        {
            config_publication_state_t pubstate = {0};
            pubstate.element_address = m_current_node_addr;
            pubstate.publish_address.type = NRF_MESH_ADDRESS_TYPE_UNICAST;
            pubstate.publish_address.value = PROVISIONER_ADDRESS;
            pubstate.appkey_index = 0;
            pubstate.frendship_credential_flag = false;
            pubstate.publish_ttl = (SMART_CITY_DEVICE_COUNT > NRF_MESH_TTL_MAX ? NRF_MESH_TTL_MAX : SMART_CITY_DEVICE_COUNT);
            pubstate.publish_period.step_num = 1;
            pubstate.publish_period.step_res = ACCESS_PUBLISH_RESOLUTION_10S;
            pubstate.retransmit_count = 1;
            pubstate.retransmit_interval = 0;
            pubstate.model_id.company_id = ACCESS_COMPANY_ID_NONE;
            pubstate.model_id.model_id = HEALTH_SERVER_MODEL_ID;
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Setting publication address for the health server to 0x%04x\n", pubstate.publish_address.value);
            retry_on_fail(config_client_model_publication_set(&pubstate));

            static const uint8_t exp_status[] = {ACCESS_STATUS_SUCCESS};
            expected_status_set(CONFIG_OPCODE_MODEL_PUBLICATION_STATUS, sizeof(exp_status), exp_status);
            break;
        }

        /* Configure subscription address for the On/Off server */
        case NODE_SETUP_CONFIG_SUBSCRIPTION_SERVICE:
        {
            uint16_t element_address = m_current_node_addr;
            nrf_mesh_address_t address = {NRF_MESH_ADDRESS_TYPE_INVALID, 0, NULL};
            address.type = NRF_MESH_ADDRESS_TYPE_GROUP;
            address.value  = current_model_id->model_id | 0xC000; // garante que será um endereço de grupo multicast
            access_model_id_t model_id;
            model_id.company_id = current_model_id->company_id;
            model_id.model_id = current_model_id->model_id;
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Set: smart city device: 0x%04x  sub addr: 0x%04x\n",element_address,address.value);
            retry_on_fail(config_client_model_subscription_add(element_address, address, model_id));

            static const uint8_t exp_status[] = {ACCESS_STATUS_SUCCESS};
            expected_status_set(CONFIG_OPCODE_MODEL_SUBSCRIPTION_STATUS, sizeof(exp_status), exp_status);
            break;
        }

        /* Configure the 1st client model to 1st server */
        case NODE_SETUP_CONFIG_PUBLICATION_SERVICE:
        {
            config_publication_state_t pubstate = {0};
            pubstate.element_address = m_current_node_addr;
            pubstate.publish_address.type = NRF_MESH_ADDRESS_TYPE_GROUP;
            pubstate.publish_address.value = current_model_id->model_id | 0xC000; // garante que será um endereço de grupo multicast
            pubstate.appkey_index = m_appkey_idx;
            pubstate.frendship_credential_flag = false;
            pubstate.publish_ttl = (SMART_CITY_DEVICE_COUNT > NRF_MESH_TTL_MAX ? NRF_MESH_TTL_MAX : SMART_CITY_DEVICE_COUNT);
            pubstate.publish_period.step_num = 0;
            pubstate.publish_period.step_res = ACCESS_PUBLISH_RESOLUTION_100MS;
            pubstate.retransmit_count = 1;
            pubstate.retransmit_interval = 0;
            pubstate.model_id.company_id = current_model_id->company_id;
            pubstate.model_id.model_id = current_model_id->model_id;
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Set: smart city device: 0x%04x  pub addr: 0x%04x\n",pubstate.element_address, pubstate.publish_address.value);
            retry_on_fail(config_client_model_publication_set(&pubstate));

            static const uint8_t exp_status[] = {ACCESS_STATUS_SUCCESS};
            expected_status_set(CONFIG_OPCODE_MODEL_PUBLICATION_STATUS, sizeof(exp_status), exp_status);
            break;
        }

        default:
            ERROR_CHECK(NRF_ERROR_NOT_FOUND);
            break;
    }
}

/**
 * This function retrieves the device key for the given address, and configures the tx and rx paths
 * of the config client model.
 */
static void setup_config_client(uint16_t target_addr)
{
    dsm_handle_t        addr_handle = DSM_HANDLE_INVALID;
    dsm_handle_t        devkey_handle = DSM_HANDLE_INVALID;
    nrf_mesh_address_t  addr;

    addr.type  = NRF_MESH_ADDRESS_TYPE_UNICAST;
    addr.value = target_addr;

    /* Provisioner helper has stored address and device keys already. Retrieve them. */
    ERROR_CHECK(dsm_address_handle_get(&addr, &addr_handle));
    ERROR_CHECK(dsm_devkey_handle_get(addr.value, &devkey_handle));

    /* Configure client to communicate with server at the given address */
    ERROR_CHECK(config_client_server_bind(devkey_handle));
    ERROR_CHECK(config_client_server_set(devkey_handle, addr_handle));
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Config client setup: devkey_handle:%d addr_handle:%d\n", devkey_handle, addr_handle);
}

/*****************************************************************************************
 * m_node_composition.composition.data é um vetor de uint8_t com a seguinte estrutura de dados
 * uint8_t * data
 * {
 *    config_composition_data_header_t,
 *    para cada elemento[]
 *    {
 *        config_composition_element_header_t,
 *        access_model_id_t.model_id[],
 *        access_model_id_t[]
 *    }
 * }
 * Queremos as listas de modelos personalizados em access_model_id_t[]
 *****************************************************************************************/
static void get_next_smart_city_model(void)
{
    do
    {
        // Se o elemento ainda não foi capturado
        if(element_header==NULL)
        {
            // Obtendo o primeiro elemento
            element_header = (config_composition_element_header_t *) ((unsigned int) m_node_composition.composition.data+sizeof(config_composition_data_header_t));
            // Obtendo o contador de modelos personalizados do elemento
            current_model_count=element_header->vendor_model_count;
            __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "Captured Element Header: ", (uint8_t*)element_header, sizeof(element_header));
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "This element has %2d personalized models\n",current_model_count);
        }
        // Se foi capturado, mas já se considerou o último modelo
        else if (current_model_count<1)
        {
            // Esgotou os modelos do elemento anterior. Avança para o próximo
            element_header = (config_composition_element_header_t *)((current_model_id==NULL) ? (unsigned int) (element_header + sizeof(config_composition_element_header_t) + element_header->sig_model_count*sizeof(current_model_id->model_id)) : (unsigned int) (++current_model_id));
            // Obtendo o contador de modelos personalizados do elemento
            current_model_count=element_header->vendor_model_count;
            __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "Captured Element Header: ", (uint8_t*)element_header, sizeof(element_header));
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "This element has %2d personalized models\n",current_model_count);
            current_model_id=NULL;
        }

        // Se o elemento for inválido (fora da estrutura da composição)
        if(element_header>=(config_composition_element_header_t*)((unsigned int) m_node_composition.composition.data + m_node_composition.len-1))
        {
            element_header=NULL;
            break;
        }

        // Se o modelo do elemento atual ainda não foi capturado e existe
        if((current_model_id==NULL) && (current_model_count>0))
        {
            // Obtendo o ID do modelo
            current_model_id=(access_model_id_t *) ((unsigned int) element_header + sizeof(config_composition_element_header_t) + element_header->sig_model_count*sizeof(current_model_id->model_id));
            current_model_count--;
        }
        // Se o modelo foi capturado e ainda existem outros
        else if(current_model_count>1)
        {
            // Avançando para o próximo modelo no elemento
            current_model_id++;
            current_model_count--;
        }
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Captured Model ID: 0x%04x \n", current_model_id->model_id);
    }while(!IS_SMART_CITY_MODEL(current_model_id->model_id) && (element_header!=NULL));

    // Se a estrutura de dados da composição acabou
    if(element_header==NULL)
    {
        // operação segura com ponteiros
        current_model_id=NULL;
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "All models parsed\n");
    }
    else
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Configurando modelo 0x%04x\n", current_model_id->model_id)
    }
}

/*************************************************************************************************/
/* Public functions */

/**
 * Proccess the config client model events, and advances the node setup state machine to the next
 * state, if expected status message is received.
 */
void node_setup_config_client_event_process(config_client_event_type_t event_type,
                                        const config_client_event_t * p_event,
                                        uint16_t length)
{
    status_check_t status;

    if (event_type == CONFIG_CLIENT_EVENT_TYPE_TIMEOUT)
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Acknowledged message status not received \n");

        if (m_retry_count)
        {
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Retry ...\n");
            m_retry_count--;
            config_step_execute();
        }
        else
        {
            // TODO: fazer pular para o próximo dispositivo
            mp_config_step = &m_idle_step;
            m_node_setup_failed_cb();
        }
    }
    else if (event_type == CONFIG_CLIENT_EVENT_TYPE_MSG && *mp_config_step != NODE_SETUP_DONE
             && m_status_checked == false)
    {
        NRF_MESH_ASSERT(p_event != NULL);
        status = check_expected_status(p_event->opcode, p_event->p_msg);
        if (status == STATUS_CHECK_PASS)
        {
            /* Save composition data for later use */
            if (p_event->opcode == CONFIG_OPCODE_COMPOSITION_DATA_STATUS)
            {
                m_node_composition.len  = length;
                m_node_composition.composition.page_number = p_event->p_msg->composition_data_status.page_number;
                memcpy(m_node_composition.composition.data, p_event->p_msg->composition_data_status.data, length - 1);
                __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "Captured Data Composition: ", m_node_composition.composition.data, m_node_composition.len);
            }

            // Queremos configurar todos os modelos de cidade inteligente
            // Se o próximo passo indica o fim da configuração
            if (*(mp_config_step+1)==NODE_SETUP_DONE)
            {
                // Verifica se ainda há modelos da cidade inteligente a serem configurados
                get_next_smart_city_model();
                if(current_model_id != NULL)
                {
                    // Se sim, configura o modelo
                    mp_config_step=smart_city_models_config_steps;
                }
                else
                {
                    // Senão, finaliza a configuração
                    mp_config_step++;
                }
            }
            else
            {
                mp_config_step++;
            }

            if (*mp_config_step == NODE_SETUP_DONE)
            {
                mp_config_step = &m_idle_step;
                m_node_setup_success_cb();
            }
            else
            {
                m_send_timer.count = CLIENT_BUSY_SEND_RETRY_LIMIT;
                config_step_execute();
            }
        }
        else if (status == STATUS_CHECK_FAIL)
        {
            mp_config_step = &m_idle_step;
            m_node_setup_failed_cb();
        }
    }
}

/**
 * Begins the node setup process.
 */
void node_setup_start(uint16_t address, uint8_t  retry_cnt, const uint8_t * p_appkey,
                      uint16_t appkey_idx)
{
    if (*mp_config_step != NODE_SETUP_IDLE)
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Cannot start. Node setup procedure is in progress.\n");
        return;
    }
    m_current_node_addr = address;
    m_retry_count = retry_cnt;
    m_send_timer.timer.cb = client_send_timer_cb;
    m_send_timer.count = CLIENT_BUSY_SEND_RETRY_LIMIT;
    mp_appkey = p_appkey;
    m_appkey_idx = appkey_idx;

    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Configuring Node: 0x%04X\n", m_current_node_addr);

    setup_config_client(m_current_node_addr);
    setup_select_steps(m_current_node_addr);
    config_step_execute();
}

void node_setup_cb_set(node_setup_successful_cb_t config_success_cb,
                       node_setup_failed_cb_t config_failed_cb)
{
    NRF_MESH_ASSERT(config_success_cb != NULL);
    NRF_MESH_ASSERT(config_failed_cb != NULL);

    m_node_setup_success_cb = config_success_cb;
    m_node_setup_failed_cb = config_failed_cb;
}
