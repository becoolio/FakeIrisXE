#ifndef _IOKIT_FakeIrisXEDisplayMergeNub_H
#define _IOKIT_FakeIrisXEDisplayMergeNub_H

#include <IOKit/IOService.h>

class FakeIrisXEDisplayMergeNub : public IOService
{
    OSDeclareDefaultStructors(FakeIrisXEDisplayMergeNub)
    
public:
    IOService *			probe(IOService *provider, SInt32 *score);
    bool                start(IOService *provider);
    virtual bool 		MergeDictionaryIntoProvider(IOService *  provider, OSDictionary *  mergeDict);
    virtual bool		MergeDictionaryIntoDictionary(OSDictionary *  sourceDictionary,  OSDictionary *  targetDictionary);

};
    
#endif
