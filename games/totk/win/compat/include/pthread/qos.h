/* Stand-in for <pthread/qos.h>: thread QoS classes are recorded but change nothing here; the port sets thread
   priorities and EcoQoS through its own platform layer. */
#pragma once
typedef enum { QOS_CLASS_UNSPECIFIED = 0, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UTILITY = 0x11,
               QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_USER_INTERACTIVE = 0x21 } qos_class_t;
static inline int pthread_set_qos_class_self_np(qos_class_t c, int p) { (void)c; (void)p; return 0; }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *c, int *p) { (void)t; *c = QOS_CLASS_USER_INTERACTIVE; *p = 0; return 0; }
