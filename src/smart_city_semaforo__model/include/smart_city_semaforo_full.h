#ifndef SMART_CITY_SEMAFORO_FULL_H__
#define SMART_CITY_SEMAFORO_FULL_H__

#include <stdint.h>
#include "access.h"
#include "smart_city_semaforo_common.h"

/** Simple Smart City Semaforo Client model ID. */
#define SMART_CITY_SEMAFORO_FULL_MODEL_ID (0xC001)

/** Forward declaration. */
typedef struct __smart_city_semaforo_full smart_city_semaforo_full_t;

/** Mensagens que serão recebidas */
/** callback type para processar mensagens do tipo SET */
typedef void (*smart_city_semaforo_set_cb_t)(const smart_city_semaforo_full_t * p_self, smart_city_semaforo_default_msg_t * msg, uint16_t src);

/** callback type para processar  mensagens tipo SHARE */
typedef void (*smart_city_semaforo_share_cb_t)(const smart_city_semaforo_full_t * p_self, smart_city_semaforo_default_msg_t * msg, uint16_t src);

/** callback type para processar  mensagens tipo GET */
typedef smart_city_semaforo_default_msg_t * (*smart_city_semaforo_get_cb_t)(const smart_city_semaforo_full_t * p_self);

/** Estrutura de dados que define o modelo */
struct __smart_city_semaforo_full
{
    /** Model handle assigned to the client. */
    access_model_handle_t model_handle;
    /** callback para mensagem do tipo set */
    smart_city_semaforo_set_cb_t set_cb;
    /** callback para mensagem do tipo share */
    smart_city_semaforo_share_cb_t share_cb;
    /** callback para mensagem do tipo GET */
    smart_city_semaforo_get_cb_t get_cb;
};

/** Inicializa o modelo */
uint32_t smart_city_semaforo_full_init(smart_city_semaforo_full_t * p_semaforo_full, uint16_t element_index);

/** Mensagens que serão geradas */
/** API da mensagem GET */
uint32_t smart_city_semaforo_get(smart_city_semaforo_full_t * p_semaforo_full);

/** API para as mensagens SET e SHARE */
uint32_t smart_city_semaforo_publish(smart_city_semaforo_full_t * p_semaforo_full, smart_city_semaforo_default_msg_t * semaforo_msg, simple_smart_city_opcode_t msg_type);

#endif /* SMART_CITY_SEMAFORO_FULL_H__ */
