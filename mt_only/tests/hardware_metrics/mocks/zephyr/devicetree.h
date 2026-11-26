#ifndef MOCK_ZEPHYR_DEVICETREE_H
#define MOCK_ZEPHYR_DEVICETREE_H
#ifndef MT_MOCK_HAS_ADC
#define MT_MOCK_HAS_ADC 0
#endif
#define DT_NODELABEL(name) 1
#define DT_PATH(name) 2
#define DT_NODE_HAS_STATUS(node,status) MT_MOCK_HAS_ADC
#define DT_NODE_HAS_PROP(node,prop) MT_MOCK_HAS_ADC
#define DT_PROP(node,prop) 17728
#endif
