#ifndef SMART_CITY_SEMAFORO_COMMON_H__
#define SMART_CITY_SEMAFORO_COMMON_H__

#include <stdint.h>
#include "access.h"

#include "simple_smart_city_common.h"

/** Macros para separa��o dos dados em "smart_city_semaforo_msg_t.data" */
#define semaforo_getstate(data) (data >> 14)
#define semaforo_getdelay(data) (data & 0x3fff)

/** Macro para uni�o dos dados */
#define semaforo_setData(estado,tempo) (((estado & 0x3) << 14) + (tempo & 0x3fff))

/** Simple Smart City Semaforo status codes. */
typedef enum
{
    /** Estado do sem�foro fechado (vermelho) */
    SEMAFORO_FECHADO=0,
    /** Estado do sem�foro em aten��o (amarelo) */
    SEMAFORO_ATENCAO=1,
    /** Estado do sem�foro aberto (verde) */
    SEMAFORO_ABERTO=2,
    /** Semaforo com falha interna */
    SEMAFORO_FALHA=3
} smart_city_semaforo_status_t;

typedef uint16_t sensor_ID_t;

/** Message format for the Simple Smart City Semaforo message. */
typedef struct __attribute((packed))
{
    basic_smart_city_msg_t basic; /** Premissas */
    sensor_ID_t sensor_ID;
    uint16_t data;    /** Estado e Tempo at� a pr�xima mudan�a de estado na forma 0xEETT TTTT TTTT TTTT*/

} smart_city_semaforo_default_msg_t;

#endif /* SMART_CITY_SEMAFORO_COMMON_H__ */
