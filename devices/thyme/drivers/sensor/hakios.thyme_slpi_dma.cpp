/*
 * Physically contiguous buffers for the SLPI's FastRPC calls.
 *
 * The FastRPC header carries the physical address of the arguments buffer (the SLPI's FastRPC
 * stream is left bypassing translation by the SMMU bring-up), so the buffers have to lie in one
 * piece below the DSP's 36-bit address limit. Two are needed: the small one for the call packet,
 * and the 8 MB remote heap that FASTRPC_RMID_INIT_CREATE_STATIC hands the DSP (the sdsp_region
 * in the device tree, 8 MB, is the same size). Each slot refuses a second buffer instead of
 * hiding ownership.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>
#include <sys/types.h>

static IOBufferMemoryDescriptor *gSlpiDma;
static IOBufferMemoryDescriptor *gSlpiHeap;

static void *
slpi_dma_alloc(IOBufferMemoryDescriptor **slot, size_t size, uint64_t *physical)
{
	IOBufferMemoryDescriptor *buffer;
	addr64_t address;

	if (*slot != NULL || size == 0 || physical == NULL) {
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
	*slot = buffer;
	return buffer->getBytesNoCopy();
}

static void
slpi_dma_free(IOBufferMemoryDescriptor **slot, void *address, size_t size)
{
	if (*slot == NULL || (*slot)->getBytesNoCopy() != address) {
		return;
	}
	(void)size;
	(*slot)->complete(kIODirectionInOut);
	(*slot)->release();
	*slot = NULL;
}

extern "C" void *
hakios_slpi_dma_alloc(size_t size, uint64_t *physical)
{
	return slpi_dma_alloc(&gSlpiDma, size, physical);
}

extern "C" void
hakios_slpi_dma_free(void *address, size_t size)
{
	slpi_dma_free(&gSlpiDma, address, size);
}

extern "C" void *
hakios_slpi_heap_alloc(size_t size, uint64_t *physical)
{
	return slpi_dma_alloc(&gSlpiHeap, size, physical);
}

extern "C" void
hakios_slpi_heap_free(void *address, size_t size)
{
	slpi_dma_free(&gSlpiHeap, address, size);
}
