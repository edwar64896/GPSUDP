#include "ma.h"

int ma_init(struct ma * ma) {
								ma->established=0;
								ma->nElements=MA_ELEMENTS;
								ma->elementPtr=0;
								ma->elementTotal=0;
								ma->elementArray=calloc(sizeof(uint64_t),MA_ELEMENTS);
}
/*
 * return the current moving average
 */
double ma_avg(struct ma * ma) {
				// if we are established then
				// return array total divided by the number of elements
				if (ma->established) {
								return (double)ma->elementTotal/(double)ma->nElements;
				} else {

								// otherwise return the total divided by the 
								// number of elements we have traversed already.
								return (double)ma->elementTotal/(double)ma->elementPtr;
				}
}

void ma_reset(struct ma * ma) {
				ma->established=0;
				ma->elementTotal=0;
				ma->elementPtr=0;
				memset(ma->elementArray,0,sizeof(uint64_t)*ma->nElements);
}
/* 
 *check to see if the element is within a certain percentage of the current average
 */
int ma_constrain(struct ma * ma, double percentage, uint64_t element) {
				double max=ma_avg(ma)*(1+percentage);
				double min=ma_avg(ma)*(1-percentage);

				if ((double)element<=max && (double)element>=min || !ma->established) 
								return 1;
				else
								return 0;
}

/* 
 * function to take a moving average of the page arrival time
 *
 * struct ma holds all the data about the moving average.
 * one of these per stream.
 *
 * output of this drive the resampler and there is one
 * resampler per stream.
 */
double ma_push( struct ma * ma, uint64_t element){

				// reduce total by the element we are replacing
				ma->elementTotal-=*(ma->elementArray+ma->elementPtr); 	

				// replace the element with a new one
				*(ma->elementArray+ma->elementPtr)=element;

				// update the total
				ma->elementTotal+=element;

				// increment the element pointer. 
				// loop around if we get to the end of the array
				ma->elementPtr=(ma->elementPtr+1) % ma->nElements;

				// if we have looped around once, we are an established array
				if (ma->elementPtr==0)
								ma->established=1;

				return ma_avg(ma);

}

