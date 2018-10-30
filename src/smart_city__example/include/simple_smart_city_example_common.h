#ifndef SIMPLE_SMART_CITY_EXAMPLE_COMMON_H__
#define SIMPLE_SMART_CITY_EXAMPLE_COMMON_H__

/**
 * @defgroup LIGHT_SWITCH_EXAMPLE_COMMON Common definitions for the Light switch example
 * @{
 */

/** Número máximo de dispositivos para esta demonstração (mantido por considerar as limitações do dispositivo)
 */
#define SMART_CITY_DEVICE_COUNT (30)
#if SMART_CITY_DEVICE_COUNT > 30
#error Maximum 30 devices currently supported by demonstration.
#endif

/** Number of group address being used in this example */
#define GROUP_ADDR_COUNT (4)

/** Chave estática de autorização */
#define STATIC_AUTH_DATA {0x6E, 0x6F, 0x72, 0x64, 0x69, 0x63, 0x5F, 0x65, 0x78, 0x61, 0x6D, 0x70, 0x6C, 0x65, 0x5F, 0x31}

/** Prefixo dos labels UUID dos dispositivos de cidade inteligente */
#define SMART_CITY_NODE_UUID_PREFIX      {'C', 'I', 'T', 'Y'}
#define SMART_CITY_NODE_UUID_PREFIX_SIZE (4)



/** @} end of Common definitions for the Light switch example */

#endif /* SIMPLE_SMART_CITY_EXAMPLE_COMMON_H__ */
