/* The native daemon discovers its public address by matching the service name
 * "bluetooth" and reading local-mac-address. H4 data uses the kernel-control
 * transport; this entry publishes only the controller's measured identity. */
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <sys/errno.h>

extern "C" int umi_bluetooth_publish(const uint8_t address[6],
    const uint8_t commands[64],uint32_t transport_speed);
static IOService *bluetooth_service;
extern "C" void umi_bluetooth_set_ready(bool ready);
extern "C" void umi_bluetooth_set_ready(bool ready)
{
    if (bluetooth_service) bluetooth_service->setProperty("HakiosBluetoothReady", ready);
}
extern "C" int
umi_bluetooth_publish(const uint8_t address[6],const uint8_t commands[64],
    uint32_t transport_speed)
{
    IOService *platform = IOService::getPlatform();
    OSDictionary *match = IOService::nameMatching("qca6390");
    IOService *node = match != NULL ? IOService::copyMatchingService(match) : NULL;
    IOService *provider = node != NULL ? node : platform;
    IOService *service = new IOService;
    if (platform == NULL || service == NULL) {
        if (service) service->release();
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(match);
        return ENOMEM;
    }
    if (!service->init()) {
        service->release();
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(match);
        return ENOMEM;
    }
    OSData *mac = OSData::withBytes(address, 6);
    if (!mac) {
        service->release();
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(match);
        return ENOMEM;
    }
    bool published = service->setProperty("local-mac-address", mac);
    mac->release();
    if (!published || !service->attach(provider)) {
        service->release();
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(match);
        return ENOMEM;
    }
    OSData *supported = OSData::withBytes(commands,64);
    if (!supported) {
        service->detach(provider);
        service->release();
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(match);
        return ENOMEM;
    }
    published = service->setProperty("HakiosBluetoothSupportedCommands",supported);
    supported->release();
    if (!published) {
        service->detach(provider);
        service->release();
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(match);
        return ENOMEM;
    }
    service->setProperty("transport-speed", transport_speed, 32);
    service->setProperty("compatible", "qca,qca6390");
    service->setProperty("HakiosBluetoothReady", false);
    service->setName("bluetooth");
    if (!service->start(provider)) {
        service->detach(provider);
        service->release();
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(match);
        return EIO;
    }
    service->registerService();
    IOLog("HakiosBluetooth: published %s under %s\n", "bluetooth",
        node != NULL ? "qca6390" : "platform");
    bluetooth_service = service;
    service->release();
    OSSafeReleaseNULL(node);
    OSSafeReleaseNULL(match);
    return 0;
}
