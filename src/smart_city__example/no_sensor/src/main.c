#include <stdint.h>
#include <string.h>

#include "boards.h"
#include "simple_hal.h"
#include "log.h"
#include "access_config.h"
#include "smart_city_semaforo_full.h"
#include "smart_city_semaforo_common.h"
#include "rtt_input.h"
#include "device_state_manager.h"
#include "simple_smart_city_example_common.h"
#include "mesh_app_utils.h"
#include "mesh_stack.h"
#include "mesh_softdevice_init.h"
#include "mesh_provisionee.h"
#include "nrf_mesh_config_examples.h"
#include "nrf_mesh_configure.h"
#include "app_timer.h"

#define MAX_DATA_STORE (8)
#define STATE_MACHINE_DELAY APP_TIMER_TICKS(1000)   // Intervalo de um segundo
#define GET_DELAY           APP_TIMER_TICKS(60000)  // Intervalo de sessenta segundos

APP_TIMER_DEF(m_timer_1s_id);
APP_TIMER_DEF(m_timer_60s_id);

static smart_city_semaforo_full_t m_semaforo_full;  // Estrutura de dados que define o modelo (ver smart_city_semaforo_full.h)
static bool m_device_provisioned;

// Estado atual do dispositivo. Valores serão inicializados após o provisionamento
static timestamp64_t timestamp;
static geolocalizador_t geolocalizador;

// Base de dados onde as informações coletadas serão armazenadas para posterior compartilhamento.
static smart_city_semaforo_default_msg_t data_store[MAX_DATA_STORE];
static bool max_data=false;
static uint8_t ler=-1,escrever=-1;

static void escrever_avanca(void)
{
    escrever++;
    if(escrever>=MAX_DATA_STORE)
    {
        escrever=0;
        max_data=true;
    }
}

static bool ler_avanca(void)
{
    if(escrever < MAX_DATA_STORE)
    {
        ler++;
        if(ler>=(max_data ? MAX_DATA_STORE : escrever))
        {
            ler=0;
        }
        return true;
    }else
    {
        ler=-1;
        return false;
    }
}

/****************************************************************************
 * Funções de Callback para tratar as informações recebidas no nível da aplicação.
 * Devem seguir os protótipos definidos em smart_city_semaforo_full.h 
 ****************************************************************************/

/** smart_city_semaforo_set_cb_t
    Esta função manipula a informação recebida diretamente do semáforo (dispositivo sensor) */
static void smart_city_semaforo_set_cb(const smart_city_semaforo_full_t * p_self, smart_city_semaforo_default_msg_t * msg, uint16_t src)
{
    escrever_avanca();
    data_store[escrever]=*msg;
    data_store[escrever].basic.geolocalizador= geolocalizador;
    data_store[escrever].basic.timestamp64[0] = timestamp[0];
    data_store[escrever].basic.timestamp64[1] = timestamp[1];// timestamp fictício
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Got a SIMPLE_SMART_CITY_SET message from t_light 0x%04x with state 0x%01x \n", msg->sensor_ID, semaforo_getstate(msg->data));
}

/** smart_city_semaforo_share_cb_t
    Esta função manipula informação recebida de outros dispositivos */
static void smart_city_semaforo_share_cb(const smart_city_semaforo_full_t * p_self, smart_city_semaforo_default_msg_t * msg, uint16_t src)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Got a SIMPLE_SMART_CITY_SHARE message from 0x%04x saying t_light 0x%04x was state 0x%01x \n", src, msg->sensor_ID, semaforo_getstate(msg->data));
    // Checagem básica de mensagem repetida. Note que a checagem é feita com a mensagem anterior somente.
    if(data_store[escrever].basic.timestamp64[1] - msg->basic.timestamp64[1])
    {
        escrever_avanca();
        data_store[escrever]=*msg;
    }else
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Message not stored \n");
    }
}

/** smart_city_semaforo_get_cb_t
    Esta função retorna o estado atual do serviço (semáforo) quando solicitado
    O dispositivo sem sensor não possui estado atual para compartilhar */
static smart_city_semaforo_default_msg_t * smart_city_semaforo_get_cb(const smart_city_semaforo_full_t * p_self)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Got a SIMPLE_SMART_CITY_GET message. Nothing to say\n");
    return NULL;
}

// Verifica se o endereço de grupo multicast está configurado
static bool semaforo_full_publication_configured(void)
{
    dsm_handle_t pub_addr_handle;
    if (access_model_publish_address_get(m_semaforo_full.model_handle, &pub_addr_handle) == NRF_SUCCESS)
    {
        if (pub_addr_handle == DSM_HANDLE_INVALID)
        {
            return false;
        }
    }
    else
    {
        return false;
    }
    return true;
}

/***************************************************************************
 * Contagem do tempo e Máquina de estado do Dispositivo.
 * Esta função deve ser invocada a cada segundo
 ***************************************************************************/
static void device_machine_state(void)
{
   // Contagem do tempo
   timestamp[1]++;
   if(!timestamp[1]) // overload do timestamp de 32 bits (que deve ocorrer em algum momento de 2038)
   {
       timestamp[0]++;
   }
   // Atualiza a posição do dispositivo
   // Ainda não implementado
}

// Função para compartilhar informação, deve ser invocada a cada segundo
static void semaforo_share(void)
{
   ler_avanca();
   if(ler<MAX_DATA_STORE)
   {   
      __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Sending a SIMPLE_SMART_CITY_SHARE message with tlight 0x%04x state eguals to 0x%02x \n", data_store[ler].sensor_ID, semaforo_getstate(data_store[ler].data));
      (void)smart_city_semaforo_publish(&m_semaforo_full,&data_store[ler],SIMPLE_SMART_CITY_SHARE);
   }else
   {
      __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "There is still no data in data_store \n");
   }
}

// Função para solicitar o estado atual do serviço
static void semaforo_get(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Requesting a SIMPLE_SMART_CITY_GET message \n");
    (void)smart_city_semaforo_get(&m_semaforo_full);
}

// Callback para o temporizador de 1 s
static void timer_handler_1s(void * p_context)
{
    device_machine_state();
    semaforo_share();
}

// Callback para temporizador de 60 s
static void timer_handler_60s(void * p_context)
{
    if(semaforo_full_publication_configured())
    {
        semaforo_get();
    }
}

// Cria o timer com a respectiva função de callback
static void timer_init(void)
{   
    ret_code_t err_code;
    err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);
    // Create timers
    err_code = app_timer_create(&m_timer_1s_id,APP_TIMER_MODE_REPEATED,timer_handler_1s);
    APP_ERROR_CHECK(err_code);
    err_code = app_timer_create(&m_timer_60s_id,APP_TIMER_MODE_REPEATED,timer_handler_60s);
    APP_ERROR_CHECK(err_code);
}

// Callback para o fim do provisionamento
static void provisioning_complete_cb(void)
{
    // inicializando o estado atual após o provisionamento do dispositivo
    geolocalizador.latitude= -15.832167;
    geolocalizador.longitude= -47.835299; // posição fictícia do dispositivo (algum lugar no DF, Brasil)
    timestamp[0] = 0x0;
    timestamp[1] = 0x5B0EED02; // Unix timestamp fictício de 64 bits (meados de 2018)
    escrever=ler=-1;

    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Successfully provisioned\n");

    dsm_local_unicast_address_t node_address;
    dsm_local_unicast_addresses_get(&node_address);
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Node Address: 0x%04x \n", node_address.address_start);

    // inicializando o temporizador da máquina de estado
    uint32_t err_code;
    err_code = app_timer_start(m_timer_1s_id,STATE_MACHINE_DELAY,NULL);
    APP_ERROR_CHECK(err_code);
    err_code = app_timer_start(m_timer_60s_id,GET_DELAY,NULL);
    APP_ERROR_CHECK(err_code);
}

static void node_reset(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "----- Node reset  -----\n");
    /* This function may return if there are ongoing flash operations. */
    mesh_stack_device_reset();
}

static void config_server_evt_cb(const config_server_evt_t * p_evt)
{
    if (p_evt->type == CONFIG_SERVER_EVT_NODE_RESET)
    {
        node_reset();
    }
}

// Inicializa o modelo
static void models_init_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Initializing and adding models\n");

    // Funções de callback
    m_semaforo_full.get_cb = smart_city_semaforo_get_cb;
    m_semaforo_full.set_cb = smart_city_semaforo_set_cb;
    m_semaforo_full.share_cb = smart_city_semaforo_share_cb;
    // inicialização do modelo
    ERROR_CHECK(smart_city_semaforo_full_init(&m_semaforo_full, 0));
    ERROR_CHECK(access_model_subscription_list_alloc(m_semaforo_full.model_handle));
}

// Inicializa a pilha de protocolos
static void mesh_init(void)
{
    uint8_t dev_uuid[NRF_MESH_UUID_SIZE];
    uint8_t node_uuid_prefix[SMART_CITY_NODE_UUID_PREFIX_SIZE] = SMART_CITY_NODE_UUID_PREFIX;

    ERROR_CHECK(mesh_app_uuid_gen(dev_uuid, node_uuid_prefix, SMART_CITY_NODE_UUID_PREFIX_SIZE));
    mesh_stack_init_params_t init_params =
    {
        .core.irq_priority       = NRF_MESH_IRQ_PRIORITY_LOWEST,
        .core.lfclksrc           = DEV_BOARD_LF_CLK_CFG,
        .core.p_uuid             = dev_uuid,
        .models.models_init_cb   = models_init_cb,
        .models.config_server_cb = config_server_evt_cb
    };
    ERROR_CHECK(mesh_stack_init(&init_params, &m_device_provisioned));
}

// inicializa o dispositivo
static void initialize(void)
{
    __LOG_INIT(LOG_SRC_APP | LOG_SRC_ACCESS, LOG_LEVEL_INFO, LOG_CALLBACK_DEFAULT);
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "----- BLE Mesh Traffic Light Full Demo -----\n");

    nrf_clock_lf_cfg_t lfc_cfg = DEV_BOARD_LF_CLK_CFG;
    ERROR_CHECK(mesh_softdevice_init(lfc_cfg));
    mesh_init();
}

static void start(void)
{
    ERROR_CHECK(mesh_stack_start());

    if (!m_device_provisioned)
    {
        static const uint8_t static_auth_data[NRF_MESH_KEY_SIZE] = STATIC_AUTH_DATA;
        mesh_provisionee_start_params_t prov_start_params =
        {
            .p_static_data    = static_auth_data,
            .prov_complete_cb = provisioning_complete_cb
        };
        ERROR_CHECK(mesh_provisionee_prov_start(&prov_start_params));
    }

    const uint8_t *p_uuid = nrf_mesh_configure_device_uuid_get();
    __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "Device UUID ", p_uuid, NRF_MESH_UUID_SIZE);
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
