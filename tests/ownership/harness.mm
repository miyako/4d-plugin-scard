/*
 Ownership regression harness for the SCARD plugin.

 Compiles the actual plugin source (4DPlugin-SCARD.cpp is #included so its
 static command functions are reachable), the actual support/4DPlugin-JSON.cpp
 and the unchanged SDK 4DPluginAPI.c, against a synthetic Call4D that models
 4D's value ownership:

   - every Unistring is a separate malloc'd buffer (ASan catches double frees)
   - PA_GetObjectProperty returns an owned copy (string) or a new reference
     (object/collection) that the caller must PA_ClearVariable
   - PA_SetObjectProperty / PA_SetCollectionElement copy or retain the value;
     the source variable still belongs to the caller
   - PA_ReturnObject hands the reference to 4D (released here after the call)

 The smart-card boundary (CryptoTokenKit, libusb, libpafe) is stubbed: no real
 reader or card is ever touched. After each case the harness asserts that no
 Unistring and no object/collection is still alive.
 */

/* keep the stub apart from the real CryptoTokenKit class loaded with the system frameworks */
#define TKSmartCardSlotManager SyntheticTKSmartCardSlotManager
#include "4DPlugin-SCARD.cpp"
#include "PrivateTypes.h"
#include "EntryPoints.h"

#include <map>
#include <set>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma mark - synthetic 4D value model

struct Value;

struct Container {
    bool isCollection;
    int refs;
    std::map<std::u16string, PA_Variable> props;
    std::vector<PA_Variable> items;
};

static std::recursive_mutex gModelMutex;
static std::set<PA_Unichar *> gLiveStrings;
static std::set<Container *> gLiveContainers;

static size_t u16len(const PA_Unichar *s) {
    size_t n = 0;
    if(s) while(s[n]) ++n;
    return n;
}

static PA_Unistring newString(const PA_Unichar *s, size_t len) {
    PA_Unistring u;
    memset(&u, 0, sizeof(u));
    PA_Unichar *buf = (PA_Unichar *)malloc((len + 1) * sizeof(PA_Unichar));
    if(len) memcpy(buf, s, len * sizeof(PA_Unichar));
    buf[len] = 0;
    u.fString = buf;
    u.fLength = (PA_long32)len;
    gLiveStrings.insert(buf);
    return u;
}

static void freeString(PA_Unistring *u) {
    PA_Unichar *buf = u->fString; /* PA_Unistring is packed: copy before binding a reference */
    if(!buf) return;
    gLiveStrings.erase(buf);
    free(buf); /* a second free of the same buffer trips ASan */
    u->fString = NULL;
    u->fLength = 0;
}

static Container *newContainer(bool isCollection) {
    Container *c = new Container();
    c->isCollection = isCollection;
    c->refs = 1;
    gLiveContainers.insert(c);
    return c;
}

static void releaseValue(PA_Variable *v);

static void releaseContainer(Container *c) {
    if(!c) return;
    if(!gLiveContainers.count(c)) {
        fprintf(stderr, "FATAL: release of dead container %p\n", (void *)c);
        abort();
    }
    if(--c->refs > 0) return;
    gLiveContainers.erase(c);
    for(auto &p : c->props) releaseValue(&p.second);
    for(auto &i : c->items) releaseValue(&i);
    delete c;
}

static void releaseValue(PA_Variable *v) {
    switch(v->fType) {
        case eVK_Unistring:
            freeString(&v->uValue.fString);
            break;
        case eVK_Object:
            releaseContainer((Container *)v->uValue.fObject);
            break;
        case eVK_Collection:
            releaseContainer((Container *)v->uValue.fCollection);
            break;
        default:
            break;
    }
    v->fType = eVK_Undefined;
}

/* the copy 4D keeps for itself (set) or hands out (get) */
static PA_Variable copyValue(const PA_Variable &src) {
    PA_Variable dst = src;
    switch(src.fType) {
        case eVK_Unistring:
            dst.uValue.fString = newString(src.uValue.fString.fString, src.uValue.fString.fLength);
            break;
        case eVK_Object:
            if(src.uValue.fObject) ((Container *)src.uValue.fObject)->refs++;
            break;
        case eVK_Collection:
            if(src.uValue.fCollection) ((Container *)src.uValue.fCollection)->refs++;
            break;
        default:
            break;
    }
    return dst;
}

static std::u16string keyOf(const PA_Unistring &u) {
    return std::u16string((const char16_t *)u.fString, (size_t)u.fLength);
}

static void setUndefined(PA_Variable *v) {
    memset(v, 0, sizeof(*v));
    v->fType = eVK_Undefined;
}

static void stubCall4D(short selector, EngineBlock *eb) {

    std::lock_guard<std::recursive_mutex> lock(gModelMutex);

    eb->fError = eER_NoErr;

    switch(selector) {

        case EX_GET_INFORMATION:
            eb->fParam2 = 0x1A00; /* >= 0x1630: PA_CreateObject uses New object */
            break;

        case EX_HANDLE_MANAGER:
            switch(eb->fParam1) {
                case 1: eb->fHandle = (PA_Handle)calloc(1, sizeof(void *) + (size_t)eb->fParam2 + 1);
                    *(size_t *)eb->fHandle = (size_t)eb->fParam2; break;
                case 2: free(eb->fHandle); break;
                case 4: eb->fParam2 = (sLONG_PTR)*(size_t *)eb->fHandle; break;
                case 5: eb->fParam3 = (sLONG_PTR)((char *)eb->fHandle + sizeof(void *)); break;
                default: break;
            }
            break;

        case EX_CREATE_UNISTRING: {
            const PA_Unichar *s = (const PA_Unichar *)eb->fHandle;
            eb->fUniString1 = newString(s, u16len(s));
            break;
        }

        case EX_DISPOSE_UNISTRING:
            freeString(&eb->fUniString1);
            break;

        case EX_VARIABLE_TO_STRING: {
            /* borrowed view of the variable's own string */
            PA_Variable *v = (PA_Variable *)eb->fHandle;
            if(v->fType == eVK_Unistring) eb->fUniString1 = v->uValue.fString;
            break;
        }

        case EX_CLEAR_VARIABLE:
            releaseValue((PA_Variable *)eb->fPtr1);
            break;

        case EX_EXECUTE_COMMAND_BY_ID: {
            PA_Variable **args = (PA_Variable **)eb->fPtr1;
            PA_Variable *ret = (PA_Variable *)eb->fPtr2;
            switch(eb->fParam1) {
                case 1471: /* New object */
                    ret->fType = eVK_Object;
                    ret->uValue.fObject = newContainer(false);
                    break;
                case 1472: /* New collection */
                    ret->fType = eVK_Collection;
                    ret->uValue.fCollection = newContainer(true);
                    break;
                case 1231: { /* OB Is defined */
                    Container *c = (Container *)args[0]->uValue.fObject;
                    ret->fType = eVK_Boolean;
                    ret->uValue.fBoolean = c && c->props.count(keyOf(args[1]->uValue.fString)) ? 1 : 0;
                    break;
                }
                case 1218: { /* JSON Parse(text; Is object): synthetic object */
                    Container *c = newContainer(false);
                    PA_Variable s;
                    s.fType = eVK_Unistring;
                    s.uValue.fString = newString(args[0]->uValue.fString.fString, args[0]->uValue.fString.fLength);
                    c->props[u"json"] = s;
                    ret->fType = eVK_Object;
                    ret->uValue.fObject = c;
                    break;
                }
                default:
                    fprintf(stderr, "FATAL: unexpected command %ld\n", (long)eb->fParam1);
                    abort();
            }
            break;
        }

        case EX_GET_OBJ_VALUE: {
            Container *c = (Container *)eb->fPtr1;
            PA_Variable *out = (PA_Variable *)eb->fPtr2;
            if(c->isCollection && eb->fLongint == -1) { /* .length */
                setUndefined(out);
                out->fType = eVK_Longint;
                out->uValue.fLongint = (PA_long32)c->items.size();
                break;
            }
            auto it = c->props.find(keyOf(eb->fUniString1));
            if(it == c->props.end()) { setUndefined(out); break; }
            *out = copyValue(it->second);
            break;
        }

        case EX_SET_OBJ_VALUE: {
            Container *c = (Container *)eb->fPtr1;
            PA_Variable *in = (PA_Variable *)eb->fPtr2;
            PA_Variable stored = copyValue(*in);
            if(c->isCollection) {
                size_t index = (size_t)eb->fLongint;
                if(index >= c->items.size()) {
                    PA_Variable u; setUndefined(&u);
                    c->items.resize(index + 1, u);
                }
                releaseValue(&c->items[index]);
                c->items[index] = stored;
            }else{
                std::u16string k = keyOf(eb->fUniString1);
                auto it = c->props.find(k);
                if(it != c->props.end()) releaseValue(&it->second);
                c->props[k] = stored;
            }
            break;
        }

        default:
            fprintf(stderr, "FATAL: unexpected Call4D selector %d\n", selector);
            abort();
    }
}

#pragma mark - synthetic smart-card boundary

static NSArray<NSString *> *gTKSlotNames = nil;

@implementation TKSmartCardSlotManager
+ (TKSmartCardSlotManager *)defaultManager {
    static TKSmartCardSlotManager *shared = nil;
    if(!shared) shared = [[TKSmartCardSlotManager alloc] init];
    return shared;
}
- (NSArray<NSString *> *)slotNames { return gTKSlotNames ? gTKSlotNames : @[]; }
- (TKSmartCardSlot *)slotNamed:(NSString *)name { return nil; }
@end

/* libusb: optionally reports one synthetic Sony RC-S380 */
static bool gUSBDevice = false;
static int gFakeDev;

extern "C" {
int libusb_init(libusb_context **) { return gUSBDevice ? 0 : LIBUSB_ERROR_OTHER; }
void libusb_exit(libusb_context *) {}
ssize_t libusb_get_device_list(libusb_context *, libusb_device ***list) {
    libusb_device **l = (libusb_device **)calloc(2, sizeof(libusb_device *));
    l[0] = (libusb_device *)&gFakeDev;
    *list = l;
    return 1;
}
void libusb_free_device_list(libusb_device **list, int) { free(list); }
int libusb_get_device_descriptor(libusb_device *, struct libusb_device_descriptor *d) {
    memset(d, 0, sizeof(*d));
    d->idVendor = LIBUSB_SONY;
    d->idProduct = LIBUSB_SONY_RC_S380;
    return 0;
}
libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *, uint16_t, uint16_t) { return NULL; }
void libusb_close(libusb_device_handle *) {}
libusb_device *libusb_get_device(libusb_device_handle *) { return NULL; }
int libusb_get_config_descriptor(libusb_device *, uint8_t, struct libusb_config_descriptor **) { return LIBUSB_ERROR_OTHER; }
void libusb_free_config_descriptor(struct libusb_config_descriptor *) {}
int libusb_set_configuration(libusb_device_handle *, int) { return LIBUSB_ERROR_OTHER; }
int libusb_claim_interface(libusb_device_handle *, int) { return LIBUSB_ERROR_OTHER; }
int libusb_release_interface(libusb_device_handle *, int) { return 0; }
int libusb_set_interface_alt_setting(libusb_device_handle *, int, int) { return LIBUSB_ERROR_OTHER; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *, int) { return 0; }
int libusb_bulk_transfer(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned int) { return LIBUSB_ERROR_OTHER; }
}

pasori *pasori_open(void) { return NULL; }
int pasori_init(pasori *) { return -1; }
void pasori_close(pasori *) {}
int pasori_reset(pasori *) { return -1; }
void pasori_set_timeout(pasori *, int) {}
felica *felica_polling(pasori *, uint16, uint8, uint8) { return NULL; }
int felica_read_single(felica *, int, int, uint8, uint8 *) { return -1; }
int felica_request_system(felica *, int *, uint16 *) { return -1; }
int felica_search_service(felica *) { return -1; }

#pragma mark - harness

static int gFailures = 0;

struct Snapshot { size_t strings, containers; };
static Snapshot snap() {
    std::lock_guard<std::recursive_mutex> lock(gModelMutex);
    return { gLiveStrings.size(), gLiveContainers.size() };
}

static void expectBalanced(const char *name, Snapshot before) {
    Snapshot after = snap();
    long ds = (long)after.strings - (long)before.strings;
    long dc = (long)after.containers - (long)before.containers;
    if(ds || dc) {
        printf("FAIL %s: leaked %ld Unistring(s), %ld object/collection(s)\n", name, ds, dc);
        gFailures++;
    }else{
        printf("PASS %s\n", name);
    }
}

static void expect(const char *name, bool ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if(!ok) gFailures++;
}

/* run a command: param 1 is a pointer to either an object ref or a Unistring */
static PA_ObjectRef call(PA_long32 selector, void *param1) {
    PA_ObjectRef result = NULL;
    void *parameters[1] = { param1 };
    PluginBlock pb;
    pb.fParameters = parameters;
    pb.fResult = &result;
    pb.fData = NULL;
    PluginMain(selector, &pb);
    return result;
}

static PA_Unistring u(const char16_t *s) {
    std::lock_guard<std::recursive_mutex> lock(gModelMutex);
    return newString((const PA_Unichar *)s, std::char_traits<char16_t>::length(s));
}

/* read stored values straight from the model, not through the helpers under test */
static const PA_Variable *rawProp(PA_ObjectRef o, const char16_t *key) {
    Container *c = (Container *)o;
    if(!c) return NULL;
    auto it = c->props.find(key);
    return it == c->props.end() ? NULL : &it->second;
}

static std::string getString(PA_ObjectRef o, const char16_t *key) {
    std::lock_guard<std::recursive_mutex> lock(gModelMutex);
    const PA_Variable *v = rawProp(o, key);
    std::string s;
    if(v && v->fType == eVK_Unistring)
        for(PA_long32 i = 0; i < v->uValue.fString.fLength; ++i) s += (char)v->uValue.fString.fString[i];
    return s;
}

static bool getBool(PA_ObjectRef o, const char16_t *key) {
    std::lock_guard<std::recursive_mutex> lock(gModelMutex);
    const PA_Variable *v = rawProp(o, key);
    return v && v->fType == eVK_Boolean && v->uValue.fBoolean;
}

static PA_ObjectRef getObject(PA_ObjectRef o, const char16_t *key) {
    std::lock_guard<std::recursive_mutex> lock(gModelMutex);
    const PA_Variable *v = rawProp(o, key);
    return v && v->fType == eVK_Object ? v->uValue.fObject : NULL;
}

static PA_CollectionRef getReaders(PA_ObjectRef status) {
    std::lock_guard<std::recursive_mutex> lock(gModelMutex);
    const PA_Variable *v = rawProp(status, u"readers");
    return v && v->fType == eVK_Collection ? v->uValue.fCollection : NULL;
}

static void releaseResult(PA_ObjectRef o) {
    std::lock_guard<std::recursive_mutex> lock(gModelMutex);
    releaseContainer((Container *)o);
}

static void test_get_readers(const char *name, NSArray<NSString *> *slots, bool usb, size_t expected) {
    gTKSlotNames = slots;
    gUSBDevice = usb;
    Snapshot before = snap();
    PA_ObjectRef options = NULL;
    PA_ObjectRef status = call(1, &options);
    PA_CollectionRef readers = getReaders(status);
    expect((std::string(name) + "/result").c_str(),
           status && readers && ((Container *)readers)->items.size() == expected);
    releaseResult(status);
    expectBalanced(name, before);
    gTKSlotNames = nil;
    gUSBDevice = false;
}

static void test_read_tag(const char *name, bool withArgs) {
    Snapshot before = snap();
    PA_ObjectRef args = NULL;
    if(withArgs) {
        args = PA_CreateObject();
        ob_set_s(args, L"slotName", "Synthetic Reader");
        ob_set_s(args, L"card", "TypeB");
        ob_set_n(args, L"timeout", 5);
        ob_set_b(args, L"system", true);
    }
    PA_ObjectRef status = call(2, &args);
    std::string uuid = getString(status, u"uuid");
    releaseResult(status);

    bool complete = false;
    std::string errorMessage;
    for(int i = 0; i < 500 && !complete; ++i) {
        PA_Unistring ustr = u(std::u16string(uuid.begin(), uuid.end()).c_str());
        PA_ObjectRef s = call(3, &ustr);
        complete = getBool(s, u"complete");
        errorMessage = getString(s, u"errorMessage");
        releaseResult(s);
        { std::lock_guard<std::recursive_mutex> lock(gModelMutex); freeString(&ustr); }
        if(!complete) usleep(10000);
    }
    expect((std::string(name) + "/complete").c_str(),
           complete && errorMessage == "TKSmartCardSlotManager::slotNamed() failed");
    if(args) releaseResult(args); /* the caller's object: still alive, so not released by the plugin */
    expectBalanced(name, before);
}

static void test_get_status_service_data(const char *name) {
    Snapshot before = snap();
    std::string uuid = "00000000-0000-0000-0000-SYNTHETIC";
    {
        std::lock_guard<std::mutex> lock(scardMutex);
        scardStorage[uuid]["complete"] = true;
        scardStorage[uuid]["success"] = true;
        scardStorage[uuid]["serviceDataJson"] = "{\"synthetic\":true}";
    }
    PA_Unistring ustr = u(u"00000000-0000-0000-0000-SYNTHETIC");
    PA_ObjectRef s = call(3, &ustr);
    PA_ObjectRef serviceData = getObject(s, u"serviceData");
    expect((std::string(name) + "/serviceData").c_str(),
           serviceData && getString(serviceData, u"json") == "{\"synthetic\":true}");
    releaseResult(s);
    { std::lock_guard<std::recursive_mutex> lock(gModelMutex); freeString(&ustr); }
    {
        std::lock_guard<std::mutex> lock(scardMutex);
        scardStorage.removeMember(uuid);
    }
    expectBalanced(name, before);
}

/* dormant helpers (built, not called by a command): returned refs borrow from the parent */
static void test_ob_get_o_c(const char *name) {
    Snapshot before = snap();
    PA_ObjectRef parent = PA_CreateObject();
    PA_ObjectRef child = PA_CreateObject();
    ob_set_s(child, L"k", "v");
    ob_set_o(parent, L"child", child);
    PA_CollectionRef coll = PA_CreateCollection();
    ob_set_c(parent, L"coll", coll);
    PA_ObjectRef o = ob_get_o(parent, L"child");
    PA_CollectionRef c = ob_get_c(parent, L"coll");
    expect((std::string(name) + "/values").c_str(),
           o == child && c == coll && getString(o, u"k") == "v");
    releaseResult(parent);
    expectBalanced(name, before);
}

static void test_scalar_getters(const char *name) {
    Snapshot before = snap();
    PA_ObjectRef o = PA_CreateObject();
    ob_set_n(o, L"n", 2.5);
    ob_set_b(o, L"b", true);
    expect((std::string(name) + "/values").c_str(),
           ob_get_n(o, L"n") == 2.5 && ob_get_b(o, L"b") && !ob_get_b(o, L"missing"));
    releaseResult(o);
    expectBalanced(name, before);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        PackInitBlock init;
        memset(&init, 0, sizeof(init));
        init.fCall4Dex = stubCall4D;
        void *data = NULL;
        FourDPackex(kInitPlugin, &init, &data, NULL);

        test_get_readers("get_readers_empty(control)", @[], false, 0);
        test_get_readers("get_readers_tk_slots", @[@"Synthetic Reader A", @"Synthetic Reader B"], false, 2);
        test_get_readers("get_readers_libusb_device", @[], true, 1);
        test_read_tag("read_tag_no_args(control)", false);
        test_read_tag("read_tag_args", true);
        test_get_status_service_data("get_status_service_data(control)");
        test_scalar_getters("ob_get_n_b(hardening)");
        test_ob_get_o_c("ob_get_o_c(dormant)");

        PluginMain(kDeinitPlugin, NULL);
    }
    Snapshot end = snap();
    printf("live at exit: %zu Unistring(s), %zu object/collection(s)\n", end.strings, end.containers);
    printf("%d failure(s)\n", gFailures);
    return gFailures ? 1 : 0;
}
