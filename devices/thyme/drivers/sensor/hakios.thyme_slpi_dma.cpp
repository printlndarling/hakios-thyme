/*
 * A physically contiguous buffer for the SLPI's FastRPC calls.
 *
 * The FastRPC header carries the physical address of the arguments buffer (the SLPI's FastRPC
 * stream is left bypassing translation by the SMMU bring-up), so the buffer has to lie in one
 * piece below the DSP's 36-bit address limit. One buffer at a time is all the sensor registry
 * bring-up needs; the allocator refuses a second one instead of hiding ownership.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>
#include <sys/types.h>

static IOBufferMemoryDescriptor *gSlpiDma;

extern "C" void *
hakios_slpi_dma_alloc(size_t size, uint64_t *physical)
{
	IOBufferMemoryDescriptor *buffer;
	addr64_t address;

	if (gSlpiDma != NULL || size == 0 || physical == NULL) {
		return NULL;
	}
	buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0xffffffffull);
	if (buffer == NULL) {
		return NULL;
	}
	if (buffer->prepare(kIODirectionInOut) != kIOReturnSuccess) {
		buffer->release();
		return NULL;
	}
	address = buffer->getPhysicalSegment(0, NULL);
	if (address == 0) {
		buffer->complete(kIODirectionInOut);
		buffer->release();
		return NULL;
	}
	*physical = (uint64_t)address;
	gSlpiDma = buffer;
	return buffer->getBytesNoCopy();
}

extern "C" void
hakios_slpi_dma_free(void *address, size_t size)
{
	if (gSlpiDma == NULL || gSlpiDma->getBytesNoCopy() != address) {
		return;
	}
	(void)size;
	gSlpiDma->complete(kIODirectionInOut);
	gSlpiDma->release();
	gSlpiDma = NULL;
}
