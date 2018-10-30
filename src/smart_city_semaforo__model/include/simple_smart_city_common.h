#ifndef SIMPLE_SMART_CITY_COMMON_H__
#define SIMPLE_SMART_CITY_COMMON_H__

#include <stdint.h>
#include "access.h"

/** Vendor specific company ID for Simple OnOff model */
#define SIMPLE_SMART_CITY_COMPANY_ID    (ACCESS_COMPANY_ID_NORDIC)

/** Simple Smart City opcodes. */
typedef enum
{
    SIMPLE_SMART_CITY_SHARE = 0xD1,		/** Compartilhar dados históricos aprendidos ou capturados pelo dispositivo */
    SIMPLE_SMART_CITY_SET = 0xD2,		/** Compartilhar uma nova leitura feita pelo dispositivo sensor, ou a mais recente */
    SIMPLE_SMART_CITY_GET = 0xD3,		/** Usado por um dispositivo para obter a última leitura feita pelo(s) dispositivo(s) sensor(es) daquele serviço */
} simple_smart_city_opcode_t;

/** Estrutura de dados da mensagem */
typedef uint32_t timestamp64_t [2]; /** Tempo codificado em 64 bits*/
typedef struct  /** Localização codificada em dois números em ponto flutuante com precisão de 32 bits cada */
{
	float longitude;
	float latitude;
}geolocalizador_t;

/** Message format for the Simple Smart City message. */
typedef struct __attribute((packed))
{
    timestamp64_t timestamp64 ; /** Tempo */
    geolocalizador_t geolocalizador;    /** Espaço */
} basic_smart_city_msg_t;

#endif /* SIMPLE_SMART_CITY_COMMON_H__ */
