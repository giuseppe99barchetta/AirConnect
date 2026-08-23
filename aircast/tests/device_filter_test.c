#include <assert.h>

#include "../src/device_filter.h"

int main(void) {
	assert(DeviceNameExcluded("Studio,Sala", "Studio"));
	assert(DeviceNameExcluded(" Studio , Sala ", "sala"));
	assert(!DeviceNameExcluded("Studio,Sala", "Studio Mini"));
	assert(!DeviceNameExcluded("", "Studio"));
	return 0;
}
