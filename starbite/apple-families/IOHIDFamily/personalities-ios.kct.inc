    "   {"
    /* IOHIDEventServiceUserClient */
    "     'IOClass' = 'IOHIDProviderPropertyMerger';"
    "     'IOProviderClass' = 'IOHIDEventService';"
    "     'IOProviderMergeProperties' = { 'HIDServiceSupport' = .true.; 'IOCFPlugInTypes' = { '7DDEECA8-A7B4-11DA-8A0E-0014519758EF' = 'IOHIDFamily.kext/PlugIns/IOHIDLib.plugin'; 'FA12FA38-6F1A-11D4-BA0C-0005028F18D5' = 'IOHIDFamily.kext/PlugIns/IOHIDLib.plugin'; }; 'IOUserClientClass' = 'IOHIDEventServiceUserClient'; };"
    "   },"
    "   {"
    /* IOHIDLibUserClient */
    "     'IOClass' = 'IOHIDProviderPropertyMerger';"
    "     'IOProviderClass' = 'IOHIDDevice';"
    "     'IOProviderMergeProperties' = { 'IOCFPlugInTypes' = { '40A57A4E-26A0-11D8-9295-000A958A2C78' = 'IOHIDFamily.kext/PlugIns/IOHIDLib.plugin'; '7DDEECA8-A7B4-11DA-8A0E-0014519758EF' = 'IOHIDFamily.kext/PlugIns/IOHIDLib.plugin'; 'FA12FA38-6F1A-11D4-BA0C-0005028F18D5' = 'IOHIDFamily.kext/PlugIns/IOHIDLib.plugin'; }; 'IOUserClientClass' = 'IOHIDLibUserClient'; };"
    "   },"
    "   {"
    /* IOHIDResource */
    "     'IOClass' = 'IOHIDResource';"
    "     'IOProviderClass' = 'IOResources';"
    "     'IOResourceMatch' = 'IOBSD';"
    "     'IOMatchCategory' = 'IOHIDResource';"
    "     'IOUserClientClass' = 'IOHIDResourceDeviceUserClient';"
    "   },"
    "   {"
    /* R1 Merge */
    "     'IOClass' = 'IOHIDProviderPropertyMerger';"
    "     'IOProviderClass' = 'IOHIDDevice';"
    "     'IOPropertyMatch' = { 'PrimaryUsage' = 27:32; 'PrimaryUsagePage' = 65280:32; };"
    "     'IOProviderMergeProperties' = { 'ProtectedAccess' = .true.; };"
    "   },"
    /* 4 personalities, OSUnserialize syntax checked */
