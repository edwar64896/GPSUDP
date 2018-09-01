#include <stdint.h>
#if !defined __MA__
#define __MA__

#define MA_ELEMENTS 50

/*
	 structure for managing moving average of clock timing
 */
struct ma {
				int established;
				uint8_t nElements;
				uint8_t elementPtr;
				uint64_t elementTotal;
				uint64_t * elementArray;
};
double ma_avg(struct ma * ma) ;
void ma_reset(struct ma * ma) ;
int ma_constrain(struct ma * ma, double percentage, uint64_t element) ;
double ma_push( struct ma * ma, uint64_t element);
#endif
