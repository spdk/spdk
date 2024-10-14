#ifndef SPDK_PRIORITY_CLASS_H
#define SPDK_PRIORITY_CLASS_H

#define NBITS_PRIORITY_CLASS 4
/* shift priority class value left by this to get the OR-mask or shift right by this after applying the priority 
class mask PRIORITY_CLASS_MASK to get the priority class as an integer
*/
#define PRIORITY_CLASS_BITS_POS (64 - NBITS_PRIORITY_CLASS)
#define PRIORITY_CLASS_MASK (0xFFFFFFFFFFFFFFFF << PRIORITY_CLASS_BITS_POS)
#define MASK_OUT_PRIORITY_CLASS (0xFFFFFFFFFFFFFFFF >> NBITS_PRIORITY_CLASS)
#define MIN_PRIORITY_CLASS 0
// #define MAX_PRIORITY_CLASS ((1 << NBITS_PRIORITY_CLASS) - 1)
#define MAX_PRIORITY_CLASS 1

#endif