#include "smart_city_semaforo_full.h"
#include "smart_city_semaforo_common.h"

#include <stdint.h>
#include <stddef.h>

#include "access.h"
#include "access_config.h"
#include "access_reliable.h"
#include "device_state_manager.h"
#include "nrf_mesh.h"
#include "nrf_mesh_assert.h"
#include "log.h"

/*****************************************************************************
 * Opcode handler callback(s)
 *****************************************************************************/

/** As mensagens de SET e SHARE  devem encaminhar os dados recebidos a aplicação para que sejam processadas.
    O processamento é feito pela função de callback definida na palicação */
static void handle_set_cb(access_model_handle_t handle, const access_message_rx_t * p_message, void * p_args)
{
    smart_city_semaforo_full_t * p_semaforo_full = p_args;
    NRF_MESH_ASSERT(p_semaforo_full->set_cb != NULL);
    smart_city_semaforo_default_msg_t * p_semaforo_msg = (smart_city_semaforo_default_msg_t *) p_message->p_data;
    p_semaforo_full->set_cb(p_semaforo_full, p_semaforo_msg, p_message->meta_data.src.value);
}

static void handle_share_cb(access_model_handle_t handle, const access_message_rx_t * p_message, void * p_args)
{
    smart_city_semaforo_full_t * p_semaforo_full = p_args;
    NRF_MESH_ASSERT(p_semaforo_full->share_cb != NULL);
    smart_city_semaforo_default_msg_t * p_semaforo_msg = (smart_city_semaforo_default_msg_t *) p_message->p_data;
    p_semaforo_full->share_cb(p_semaforo_full, p_semaforo_msg, p_message->meta_data.src.value);
}

/** Ao receber uma mensagem tipo GET, o dispositivo sensor deve responder com a última leitura. 
    Opcionalemnte pode-se fazer uma nova leitura neste momento.
    Essa decisão deve ser tomada dentro da função de callback definida pela aplicação.
    A resposta é feita por meio de uma mensagem do tipo SIMPLE_SMART_CITY_SET */
static void handle_get_cb(access_model_handle_t handle, const access_message_rx_t * p_message, void * p_args)
{
    smart_city_semaforo_full_t * p_semaforo_full = p_args;
    NRF_MESH_ASSERT(p_semaforo_full->get_cb != NULL);
    smart_city_semaforo_publish(p_semaforo_full, p_semaforo_full->get_cb(p_semaforo_full),SIMPLE_SMART_CITY_SET);
}

static const access_opcode_handler_t m_opcode_handlers[] =
{
    {{SIMPLE_SMART_CITY_SHARE, SIMPLE_SMART_CITY_COMPANY_ID}, handle_share_cb},
    {{SIMPLE_SMART_CITY_SET, SIMPLE_SMART_CITY_COMPANY_ID}, handle_set_cb},
    {{SIMPLE_SMART_CITY_GET, SIMPLE_SMART_CITY_COMPANY_ID}, handle_get_cb}
};

/*****************************************************************************
 * Public API: Funções que poderão ser usadas para uso do Modelo
 *****************************************************************************/

uint32_t smart_city_semaforo_full_init(smart_city_semaforo_full_t * p_semaforo_full, uint16_t element_index)
{
    // checa se a estrutura de dados que define o modelo foi corretamente configurada
    if (p_semaforo_full == NULL ||
        p_semaforo_full->get_cb == NULL ||
        p_semaforo_full->set_cb == NULL ||
        p_semaforo_full->share_cb == NULL)
    {
        return NRF_ERROR_NULL;
    }

    // ParÃ¢mentros para associar o modelo ao elemento na camada de acesso
    access_model_add_params_t init_params;
    init_params.model_id.model_id = SMART_CITY_SEMAFORO_FULL_MODEL_ID;
    init_params.model_id.company_id = SIMPLE_SMART_CITY_COMPANY_ID;
    init_params.element_index = element_index;
    init_params.p_opcode_handlers = &m_opcode_handlers[0];
    init_params.opcode_count = sizeof(m_opcode_handlers) / sizeof(m_opcode_handlers[0]);
    init_params.p_args = p_semaforo_full;
    init_params.publish_timeout_cb = NULL; // Todas as mensagens serão geradas no modo sem confirmação, por isso não precisamos lidar com timeouts
    return access_model_add(&init_params, &p_semaforo_full->model_handle);
}

uint32_t smart_city_semaforo_get(smart_city_semaforo_full_t * p_semaforo_full)
{
    access_message_tx_t message;
    message.opcode.opcode = SIMPLE_SMART_CITY_GET;
    message.opcode.company_id = SIMPLE_SMART_CITY_COMPANY_ID;
    message.p_buffer = NULL; // Mensagem do tipo GET não possui conteúdo
    message.length = 0;
    message.force_segmented = false;
    message.transmic_size = NRF_MESH_TRANSMIC_SIZE_DEFAULT;
    return access_model_publish(p_semaforo_full->model_handle, &message);
}

uint32_t smart_city_semaforo_publish(smart_city_semaforo_full_t * p_semaforo_full, smart_city_semaforo_default_msg_t * semaforo_msg, simple_smart_city_opcode_t msg_type)
{
    if(semaforo_msg==NULL)
    {
        return NRF_ERROR_NULL;
    }
    access_message_tx_t message;
    message.opcode.opcode = msg_type;
    message.opcode.company_id = SIMPLE_SMART_CITY_COMPANY_ID;
    message.p_buffer = (const uint8_t*) semaforo_msg;
    message.length = sizeof(smart_city_semaforo_default_msg_t);
    message.force_segmented = false;
    message.transmic_size = NRF_MESH_TRANSMIC_SIZE_DEFAULT;
    return access_model_publish(p_semaforo_full->model_handle, &message);
}
