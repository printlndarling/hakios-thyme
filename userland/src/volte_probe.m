#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <dlfcn.h>
#import <math.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

typedef struct {
    const uint8_t *data;
    uint32_t size;
    AudioStreamPacketDescription pd;
    int done;
} OnePacket;

static OSStatus feed_one(AudioConverterRef conv, UInt32 *ioNumberDataPackets,
                         AudioBufferList *ioData,
                         AudioStreamPacketDescription **outDataPacketDescription,
                         void *inUserData) {
    (void)conv;
    OnePacket *p = (OnePacket *)inUserData;
    if (p->done) {
        *ioNumberDataPackets = 0;
        return 0;
    }
    p->done = 1;
    ioData->mNumberBuffers = 1;
    ioData->mBuffers[0].mNumberChannels = 1;
    ioData->mBuffers[0].mDataByteSize = p->size;
    ioData->mBuffers[0].mData = (void *)p->data;
    *ioNumberDataPackets = 1;
    if (outDataPacketDescription) {
        p->pd.mStartOffset = 0;
        p->pd.mVariableFramesInPacket = 0;
        p->pd.mDataByteSize = p->size;
        *outDataPacketDescription = &p->pd;
    }
    return 0;
}

static OSStatus feed_pcm(AudioConverterRef conv, UInt32 *ioNumberDataPackets,
                         AudioBufferList *ioData,
                         AudioStreamPacketDescription **outDataPacketDescription,
                         void *inUserData) {
    (void)conv;
    (void)outDataPacketDescription;
    OnePacket *p = (OnePacket *)inUserData;
    if (p->done) {
        *ioNumberDataPackets = 0;
        return 0;
    }
    p->done = 1;
    ioData->mNumberBuffers = 1;
    ioData->mBuffers[0].mNumberChannels = 1;
    ioData->mBuffers[0].mDataByteSize = p->size;
    ioData->mBuffers[0].mData = (void *)p->data;
    *ioNumberDataPackets = p->size / 2;
    return 0;
}

static void probe_codec(const char *name, AudioFormatID fmt, Float64 rate, UInt32 fpp) {
    AudioStreamBasicDescription pcm = {0};
    pcm.mSampleRate = rate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    pcm.mBitsPerChannel = 16;
    pcm.mChannelsPerFrame = 1;
    pcm.mBytesPerFrame = 2;
    pcm.mFramesPerPacket = 1;
    pcm.mBytesPerPacket = 2;

    AudioStreamBasicDescription cmp = {0};
    cmp.mSampleRate = rate;
    cmp.mFormatID = fmt;
    cmp.mChannelsPerFrame = 1;
    cmp.mFramesPerPacket = fpp;

    AudioConverterRef enc = NULL, dec = NULL;
    OSStatus es = AudioConverterNew(&pcm, &cmp, &enc);
    OSStatus ds = AudioConverterNew(&cmp, &pcm, &dec);
    printf("volte_probe: codec %s (rate=%.0f fpp=%u): enc=%d dec=%d\n",
           name, rate, (unsigned)fpp, (int)es, (int)ds);

    if (es == 0 && enc) {
        int16_t tone[320];
        UInt32 n = fpp > 320 ? 320 : fpp;
        for (UInt32 i = 0; i < n; i++)
            tone[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * i / rate));
        OnePacket pin = { (const uint8_t *)tone, n * 2, {0}, 0 };
        uint8_t outbuf[128] = {0};
        AudioBufferList abl = {0};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 1;
        abl.mBuffers[0].mDataByteSize = sizeof(outbuf);
        abl.mBuffers[0].mData = outbuf;
        UInt32 pkts = 1;
        AudioStreamPacketDescription opd = {0};
        OSStatus er = AudioConverterFillComplexBuffer(enc, feed_pcm, &pin, &pkts, &abl, &opd);
        printf("volte_probe:   %s encode rc=%d pkts=%u bytes=%u head=%02x %02x %02x %02x %02x %02x\n",
               name, (int)er, (unsigned)pkts, (unsigned)opd.mDataByteSize,
               outbuf[0], outbuf[1], outbuf[2], outbuf[3], outbuf[4], outbuf[5]);

        if (er == 0 && pkts > 0 && ds == 0 && dec) {
            OnePacket din = { outbuf, opd.mDataByteSize, {0}, 0 };
            int16_t dpcm[320] = {0};
            AudioBufferList dabl = {0};
            dabl.mNumberBuffers = 1;
            dabl.mBuffers[0].mNumberChannels = 1;
            dabl.mBuffers[0].mDataByteSize = n * 2;
            dabl.mBuffers[0].mData = dpcm;
            UInt32 dpkts = n;
            OSStatus dr = AudioConverterFillComplexBuffer(dec, feed_one, &din, &dpkts, &dabl, NULL);
            int peak = 0;
            for (UInt32 i = 0; i < dpkts && i < 320; i++) {
                int v = abs((int)dpcm[i]);
                if (v > peak) peak = v;
            }
            printf("volte_probe:   %s decode rc=%d frames=%u peak=%d\n",
                   name, (int)dr, (unsigned)dpkts, peak);
        }
    }
    if (enc) AudioConverterDispose(enc);
    if (dec) AudioConverterDispose(dec);
    fflush(stdout);
}

static void list_audio_codecs(void) {
    AudioComponentDescription desc = {0};
    for (int kind = 0; kind < 2; kind++) {
        desc.componentType = kind == 0 ? kAudioDecoderComponentType : kAudioEncoderComponentType;
        AudioComponent comp = NULL;
        while ((comp = AudioComponentFindNext(comp, &desc)) != NULL) {
            AudioComponentDescription cd = {0};
            AudioComponentGetDescription(comp, &cd);
            char st[5] = { (char)(cd.componentSubType >> 24), (char)(cd.componentSubType >> 16),
                           (char)(cd.componentSubType >> 8), (char)cd.componentSubType, 0 };
            if (cd.componentSubType == kAudioFormatAMR || cd.componentSubType == kAudioFormatAMR_WB ||
                cd.componentSubType == kAudioFormatULaw || cd.componentSubType == kAudioFormatALaw ||
                cd.componentSubType == 'evs ' || cd.componentSubType == 'EVS ') {
                printf("volte_probe: component %s sub='%s' (0x%08x)\n",
                       kind == 0 ? "adec" : "aenc", st, (unsigned)cd.componentSubType);
            }
        }
    }
    fflush(stdout);
}

static int run_dial(const char *number, int secs) {
    void *tu = dlopen("/System/Library/PrivateFrameworks/TelephonyUtilities.framework/TelephonyUtilities", RTLD_NOW);
    printf("volte_probe: dlopen TelephonyUtilities=%p\n", tu);
    if (!tu) {
        printf("volte_probe: dlerror=%s\n", dlerror());
        return 1;
    }
    Class cCenter = objc_getClass("TUCallCenter");
    Class cReq = objc_getClass("TUDialRequest");
    Class cHandle = objc_getClass("TUHandle");
    printf("volte_probe: classes center=%p req=%p handle=%p\n", cCenter, cReq, cHandle);
    if (!cCenter || !cReq || !cHandle) return 1;

    id center = ((id(*)(id, SEL))objc_msgSend)((id)cCenter, sel_registerName("sharedInstance"));
    id pm = ((id(*)(id, SEL))objc_msgSend)(center, sel_registerName("providerManager"));
    id prov = ((id(*)(id, SEL))objc_msgSend)(pm, sel_registerName("telephonyProvider"));
    printf("volte_probe: center=%p pm=%p prov=%p\n", center, pm, prov);
    if (!prov) return 1;

    id req = ((id(*)(id, SEL, id))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)((id)cReq, sel_registerName("alloc")),
        sel_registerName("initWithProvider:"), prov);
    NSString *numStr = [NSString stringWithUTF8String:number];
    id handle = ((id(*)(id, SEL, NSInteger, id))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)((id)cHandle, sel_registerName("alloc")),
        sel_registerName("initWithType:value:"), (NSInteger)2, numStr);
    ((void(*)(id, SEL, id))objc_msgSend)(req, sel_registerName("setHandle:"), handle);
    ((void(*)(id, SEL, BOOL))objc_msgSend)(req, sel_registerName("setShowUIPrompt:"), NO);
    ((void(*)(id, SEL, BOOL))objc_msgSend)(req, sel_registerName("setPerformDialAssist:"), NO);
    NSArray *sids = ((id(*)(id, SEL))objc_msgSend)(prov, sel_registerName("prioritizedSenderIdentities"));
    if ([sids count] > 0) {
        id sid = [sids objectAtIndex:0];
        id suuid = ((id(*)(id, SEL))objc_msgSend)(sid, sel_registerName("UUID"));
        id auuid = ((id(*)(id, SEL))objc_msgSend)(sid, sel_registerName("accountUUID"));
        if (suuid) ((void(*)(id, SEL, id))objc_msgSend)(req, sel_registerName("setLocalSenderIdentityUUID:"), suuid);
        if (auuid) ((void(*)(id, SEL, id))objc_msgSend)(req, sel_registerName("setLocalSenderIdentityAccountUUID:"), auuid);
        printf("volte_probe: senderIdentity=%s\n", [[sid description] UTF8String]);
    }
    printf("volte_probe: dialing %s for %d s (req=%s)\n", number, secs, [[req description] UTF8String]);
    fflush(stdout);

    void (^cb)(id, NSError *) = ^(id call, NSError *err) {
        printf("volte_probe: dial completion call=%s err=%s\n",
               call ? [[call description] UTF8String] : "(null)",
               err ? [[err description] UTF8String] : "(null)");
        fflush(stdout);
    };
    ((void(*)(id, SEL, id, id))objc_msgSend)(center, sel_registerName("dialWithRequest:completion:"), req, cb);

    for (int i = 0; i < secs; i++) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        NSArray *calls = ((id(*)(id, SEL))objc_msgSend)(center, sel_registerName("currentAudioAndVideoCalls"));
        if (!calls) calls = ((id(*)(id, SEL))objc_msgSend)(center, sel_registerName("currentCalls"));
        printf("volte_probe: t+%02ds calls=%lu", i + 1, (unsigned long)[calls count]);
        for (id c in calls) {
            int st = ((int(*)(id, SEL))objc_msgSend)(c, sel_registerName("status"));
            double dur = ((double(*)(id, SEL))objc_msgSend)(c, sel_registerName("callDuration"));
            int disc = ((int(*)(id, SEL))objc_msgSend)(c, sel_registerName("disconnectedReason"));
            printf(" [status=%d dur=%.1f disc=%d]", st, dur, disc);
        }
        printf("\n");
        fflush(stdout);
    }

    printf("volte_probe: disconnecting all calls\n");
    fflush(stdout);
    ((void(*)(id, SEL))objc_msgSend)(center, sel_registerName("disconnectAllCalls"));
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2.5]];
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc >= 2 && strcmp(argv[1], "codec") == 0) {
            list_audio_codecs();
            probe_codec("AMR-WB", kAudioFormatAMR_WB, 16000.0, 320);
            probe_codec("AMR-NB", kAudioFormatAMR, 8000.0, 160);
            probe_codec("ULAW", kAudioFormatULaw, 8000.0, 1);
            return 0;
        }
        if (argc >= 3 && strcmp(argv[1], "dial") == 0) {
            int secs = argc >= 4 ? atoi(argv[3]) : 15;
            return run_dial(argv[2], secs);
        }
        printf("usage: volte_probe codec | dial <number> [secs]\n");
        return 1;
    }
}
