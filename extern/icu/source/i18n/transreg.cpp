/*
**********************************************************************
*   Copyright (c) 2001-2004, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*   Date        Name        Description
*   08/10/2001  aliu        Creation.
**********************************************************************
*/

#include "unicode/utypes.h"

#if !UCONFIG_NO_TRANSLITERATION

#include "unicode/utypes.h"
#include "unicode/uobject.h"
#include "unicode/parseerr.h"
#include "unicode/resbund.h"
#include "unicode/translit.h"
#include "unicode/uniset.h"
#include "unicode/uscript.h"
#include "rbt.h"
#include "cpdtrans.h"
#include "nultrans.h"
#include "transreg.h"
#include "rbt_data.h"
#include "rbt_pars.h"
#include "tridpars.h"
#include "charstr.h"
#include "uassert.h"

// Enable the following symbol to add debugging code that tracks the
// allocation, deletion, and use of Entry objects.  BoundsChecker has
// reported dangling pointer errors with these objects, but I have
// been unable to confirm them.  I suspect BoundsChecker is getting
// confused with pointers going into and coming out of a UHashtable,
// despite the hinting code that is designed to help it.
// #define DEBUG_MEM
#ifdef DEBUG_MEM
#include <stdio.h>
#endif

// UChar constants
static const UChar LOCALE_SEP  = 95; // '_'
static const UChar ID_SEP      = 0x002D; /*-*/
static const UChar VARIANT_SEP = 0x002F; // '/'

// String constants
static const UChar NO_VARIANT[] = { 0 }; // empty string
static const UChar ANY[] = { 65, 110, 121, 0 }; // Any

/**
 * Resource bundle key for the RuleBasedTransliterator rule.
 */
static const char RB_RULE[] = "Rule";

U_NAMESPACE_BEGIN

//------------------------------------------------------------------
// Alias
//------------------------------------------------------------------

TransliteratorAlias::TransliteratorAlias(const UnicodeString& theAliasID) :
    ID(),
    aliasID(theAliasID),
    trans(0),
    compoundFilter(0),
    idSplitPoint(-1),
    type(TransliteratorAlias::SIMPLE) {
}

TransliteratorAlias::TransliteratorAlias(const UnicodeString& theID,
                                         const UnicodeString& idBlock,
                                         Transliterator* adopted,
                                         int32_t theIDSplitPoint,
                                         const UnicodeSet* cpdFilter) :
    ID(theID),
    aliasID(idBlock),
    trans(adopted),
    compoundFilter(cpdFilter),
    idSplitPoint(theIDSplitPoint),
    type(TransliteratorAlias::COMPOUND) {
}

TransliteratorAlias::TransliteratorAlias(const UnicodeString& theID,
                                         const UnicodeString& rules,
                                         UTransDirection dir) :
    ID(theID),
    aliasID(rules), // bad name -- rename aliasID!
    trans(0),
    compoundFilter(0),
    idSplitPoint((int32_t) dir), // bad name -- rename idSplitPoint!
    type(TransliteratorAlias::RULES) {
}

TransliteratorAlias::~TransliteratorAlias() {
    delete trans;
}


Transliterator* TransliteratorAlias::create(UParseError& pe,
                                            UErrorCode& ec) {
    if (U_FAILURE(ec)) {
        return 0;
    }
    Transliterator *t = NULL;
    switch (type) {
    case SIMPLE:
        t = Transliterator::createInstance(aliasID, UTRANS_FORWARD, pe, ec);
        break;
    case COMPOUND:
        t = new CompoundTransliterator(ID, aliasID, idSplitPoint,
                                       trans, ec);
        /* test for NULL */
        if (t == 0) {
            ec = U_MEMORY_ALLOCATION_ERROR;
            return 0;
        }
        trans = 0; // so we don't delete it later
        if (compoundFilter) {
            // TODO: Is this right? Are we leaking memory here?
            // I'm suspicious because of the "trans = 0" line above;
            // doesn't seem to fit the cloning here.  Don't have time
            // to track this down right now. [alan 3.0]
            t->adoptFilter((UnicodeSet*) compoundFilter->clone());
        }
        break;
    case RULES:
        U_ASSERT(FALSE); // don't call create() if isRuleBased() returns TRUE!
        break;
    }
    return t;
}

UBool TransliteratorAlias::isRuleBased() const {
    return type == RULES;
}

void TransliteratorAlias::parse(TransliteratorParser& parser,
                                UParseError& pe, UErrorCode& ec) const {
    U_ASSERT(type == RULES);
    if (U_FAILURE(ec)) {
        return;
    }

    // aliasID is really rules -- rename it!
    // idSplitPoint is really UTransDirection -- rename it!
    parser.parse(aliasID, (UTransDirection) idSplitPoint, pe, ec);
}

//----------------------------------------------------------------------
// class Spec
//----------------------------------------------------------------------

/**
 * A Spec is a string specifying either a source or a target.  In more
 * general terms, it may also specify a variant, but we only use the
 * Spec class for sources and targets.
 *
 * A Spec may be a locale or a script.  If it is a locale, it has a
 * fallback chain that goes xx_YY_ZZZ -> xx_YY -> xx -> ssss, where
 * ssss is the script mapping of xx_YY_ZZZ.  The Spec API methods
 * hasFallback(), next(), and reset() iterate over this fallback
 * sequence.
 *
 * The Spec class canonicalizes itself, so the locale is put into
 * canonical form, or the script is transformed from an abbreviation
 * to a full name.
 */
class Spec : public UMemory {
 public:
    Spec(const UnicodeString& spec);
    ~Spec();

    const UnicodeString& get() const;
    UBool hasFallback() const;
    const UnicodeString& next();
    void reset();

    UBool isLocale() const;
    ResourceBundle& getBundle() const;

    operator const UnicodeString&() const { return get(); }
    const UnicodeString& getTop() const { return top; }

 private:
    void setupNext();

    UnicodeString top;
    UnicodeString spec;
    UnicodeString nextSpec;
    UnicodeString scriptName;
    UBool isSpecLocale; // TRUE if spec is a locale
    UBool isNextLocale; // TRUE if nextSpec is a locale
    ResourceBundle* res;

    Spec(const Spec &other); // forbid copying of this class
    Spec &operator=(const Spec &other); // forbid copying of this class
};

Spec::Spec(const UnicodeString& theSpec) : top(theSpec) {
    UErrorCode status = U_ZERO_ERROR;
    CharString topch(top);
    Locale toploc(topch);
    res = new ResourceBundle(u_getDataDirectory(), toploc, status);
    /* test for NULL */
    if (res == 0) {
        return;
    }
    if (U_FAILURE(status) || status == U_USING_DEFAULT_WARNING) {
        delete res;
        res = 0;
    }

    // Canonicalize script name -or- do locale->script mapping
    status = U_ZERO_ERROR;
    const int32_t capacity = 10;
    UScriptCode script[capacity]={USCRIPT_INVALID_CODE};
    int32_t num = uscript_getCode(topch,script,capacity, &status);
    if (num > 0 && script[0] != USCRIPT_INVALID_CODE) {
        scriptName = UnicodeString(uscript_getName(script[0]), "");
    }

    // Canonicalize top
    char buf[256];
    if (res != 0) {
        // Canonicalize locale name
        status = U_ZERO_ERROR;
        uloc_getName(topch, buf, sizeof(buf), &status);
        if (U_SUCCESS(status) && status != U_STRING_NOT_TERMINATED_WARNING) {
            top = UnicodeString(buf, "");
        }
    } else if (scriptName.length() != 0) {
        // We are a script; use canonical name
        top = scriptName;
    }

    // assert(spec != top);
    reset();
}

Spec::~Spec() {
    delete res;
}

UBool Spec::hasFallback() const {
    return nextSpec.length() != 0;
}

void Spec::reset() {
    if (spec != top) {
        spec = top;
        isSpecLocale = (res != 0);
        setupNext();
    }
}

void Spec::setupNext() {
    isNextLocale = FALSE;
    if (isSpecLocale) {
        nextSpec = spec;
        int32_t i = nextSpec.lastIndexOf(LOCALE_SEP);
        // If i == 0 then we have _FOO, so we fall through
        // to the scriptName.
        if (i > 0) {
            nextSpec.truncate(i);
            isNextLocale = TRUE;
        } else {
            nextSpec = scriptName; // scriptName may be empty
        }
    } else {
        // spec is a script, so we are at the end
        nextSpec.truncate(0);
    }
}

// Protocol:
// for(const UnicodeString& s(spec.get());
//     spec.hasFallback(); s(spec.next())) { ...

const UnicodeString& Spec::next() {
    spec = nextSpec;
    isSpecLocale = isNextLocale;
    setupNext();
    return spec;
}

const UnicodeString& Spec::get() const {
    return spec;
}

UBool Spec::isLocale() const {
    return isSpecLocale;
}

ResourceBundle& Spec::getBundle() const {
    return *res;
}

//----------------------------------------------------------------------

#ifdef DEBUG_MEM

// Vector of Entry pointers currently in use
static UVector* DEBUG_entries = NULL;

static void DEBUG_setup() {
    if (DEBUG_entries == NULL) {
        UErrorCode ec = U_ZERO_ERROR;
        DEBUG_entries = new UVector(ec);
    }
}

// Caller must call DEBUG_setup first.  Return index of given Entry,
// if it is in use (not deleted yet), or -1 if not found.
static int DEBUG_findEntry(Entry* e) {
    for (int i=0; i<DEBUG_entries->size(); ++i) {
        if (e == (Entry*) DEBUG_entries->elementAt(i)) {
            return i;
        }
    }
    return -1;
}

// Track object creation
static void DEBUG_newEntry(Entry* e) {
    DEBUG_setup();
    if (DEBUG_findEntry(e) >= 0) {
        // This should really never happen unless the heap is broken
        printf("ERROR DEBUG_newEntry duplicate new pointer %08X\n", e);
        return;
    }
    UErrorCode ec = U_ZERO_ERROR;
    DEBUG_entries->addElement(e, ec);
}

// Track object deletion
static void DEBUG_delEntry(Entry* e) {
    DEBUG_setup();
    int i = DEBUG_findEntry(e);
    if (i < 0) {
        printf("ERROR DEBUG_delEntry possible double deletion %08X\n", e);
        return;
    }
    DEBUG_entries->removeElementAt(i);
}

// Track object usage
static void DEBUG_useEntry(Entry* e) {
    if (e == NULL) return;
    DEBUG_setup();
    int i = DEBUG_findEntry(e);
    if (i < 0) {
        printf("ERROR DEBUG_useEntry possible dangling pointer %08X\n", e);
    }
}

#else
// If we're not debugging then make these macros into NOPs
#define DEBUG_newEntry(x)
#define DEBUG_delEntry(x)
#define DEBUG_useEntry(x)
#endif

//----------------------------------------------------------------------
// class Entry
//----------------------------------------------------------------------

/**
 * The Entry object stores objects of different types and
 * singleton objects as placeholders for rule-based transliterators to
 * be built as needed.  Instances of this struct can be placeholders,
 * can represent prototype transliterators to be cloned, or can
 * represent TransliteratorData objects.  We don't support storing
 * classes in the registry because we don't have the rtti infrastructure
 * for it.  We could easily add this if there is a need for it in the
 * future.
 */
class Entry : public UMemory {
public:
    enum Type {
        RULES_FORWARD,
        RULES_REVERSE,
        LOCALE_RULES,
        PROTOTYPE,
        RBT_DATA,
        COMPOUND_RBT,
        ALIAS,
        FACTORY,
        NONE // Only used for uninitialized entries
    } entryType;
    // NOTE: stringArg cannot go inside the union because
    // it has a copy constructor
    UnicodeString stringArg; // For RULES_*, ALIAS, COMPOUND_RBT
    int32_t intArg; // For COMPOUND_RBT, LOCALE_RULES
    UnicodeSet* compoundFilter; // For COMPOUND_RBT
    union {
        Transliterator* prototype; // For PROTOTYPE
        TransliterationRuleData* data; // For RBT_DATA, COMPOUND_RBT
        struct {
            Transliterator::Factory function;
            Transliterator::Token   context;
        } factory; // For FACTORY
    } u;
    Entry();
    ~Entry();
    void adoptPrototype(Transliterator* adopted);
    void setFactory(Transliterator::Factory factory,
                    Transliterator::Token context);

private:

    Entry(const Entry &other); // forbid copying of this class
    Entry &operator=(const Entry &other); // forbid copying of this class
};

Entry::Entry() {
    u.prototype = 0;
    compoundFilter = NULL;
    entryType = NONE;
    DEBUG_newEntry(this);
}

Entry::~Entry() {
    DEBUG_delEntry(this);
    if (entryType == PROTOTYPE) {
        delete u.prototype;
    } else if (entryType == RBT_DATA || entryType == COMPOUND_RBT) {
        // The data object is shared between instances of RBT.  The
        // entry object owns it.  It should only be deleted when the
        // transliterator component is being cleaned up.  Doing so
        // invalidates any RBTs that the user has instantiated.
        delete u.data;
    }
    delete compoundFilter;
}

void Entry::adoptPrototype(Transliterator* adopted) {
    if (entryType == PROTOTYPE) {
        delete u.prototype;
    }
    entryType = PROTOTYPE;
    u.prototype = adopted;
}

void Entry::setFactory(Transliterator::Factory factory,
                       Transliterator::Token context) {
    if (entryType == PROTOTYPE) {
        delete u.prototype;
    }
    entryType = FACTORY;
    u.factory.function = factory;
    u.factory.context = context;
}

// UObjectDeleter for Hashtable::setValueDeleter
U_CDECL_BEGIN
static void U_EXPORT2 U_CALLCONV
deleteEntry(void* obj) {
    delete (Entry*) obj;
}
U_CDECL_END

//----------------------------------------------------------------------
// class TransliteratorRegistry: Basic public API
//----------------------------------------------------------------------

TransliteratorRegistry::TransliteratorRegistry(UErrorCode& status) :
    registry(TRUE),
    specDAG(TRUE),
    availableIDs(status)
{
    registry.setValueDeleter(deleteEntry);
    availableIDs.setDeleter(uhash_deleteUnicodeString);
    availableIDs.setComparer(uhash_compareCaselessUnicodeString);
    specDAG.setValueDeleter(uhash_deleteHashtable);
}

TransliteratorRegistry::~TransliteratorRegistry() {
    // Through the magic of C++, everything cleans itself up
}

Transliterator* TransliteratorRegistry::get(const UnicodeString& ID,
                                            TransliteratorAlias*& aliasReturn,
                                            UParseError& parseError,
                                            UErrorCode& status) {
    U_ASSERT(aliasReturn == NULL);
    Entry *entry = find(ID);
    return (entry == 0) ? 0
        : instantiateEntry(ID, entry, aliasReturn, parseError,status);
}

Transliterator* TransliteratorRegistry::reget(const UnicodeString& ID,
                                              TransliteratorParser& parser,
                                              TransliteratorAlias*& aliasReturn,
                                              UParseError& parseError,
                                              UErrorCode& status) {
    U_ASSERT(aliasReturn == NULL);
    Entry *entry = find(ID);

    if (entry == 0) {
        // We get to this point if there are two threads, one of which
        // is instantiating an ID, and another of which is removing
        // the same ID from the registry, and the timing is just right.
        return 0;
    }

    // The usage model for the caller is that they will first call
    // reg->get() inside the mutex, they'll get back an alias, they call
    // alias->isRuleBased(), and if they get TRUE, they call alias->parse()
    // outside the mutex, then reg->reget() inside the mutex again.  A real
    // mess, but it gets things working for ICU 3.0. [alan].

    // Note: It's possible that in between the caller calling
    // alias->parse() and reg->reget(), that another thread will have
    // called reg->reget(), and the entry will already have been fixed up.
    // We have to detect this so we don't stomp over existing entry
    // data members and potentially leak memory (u.data and compoundFilter).

    if (entry->entryType == Entry::RULES_FORWARD ||
        entry->entryType == Entry::RULES_REVERSE ||
        entry->entryType == Entry::LOCALE_RULES) {
        
        entry->u.data = parser.orphanData();
        entry->stringArg = parser.idBlock;
        entry->intArg = parser.idSplitPoint;
        entry->compoundFilter = parser.orphanCompoundFilter();

        // Reset entry->entryType to encapsulate the parsed data.  The
        // next time we instantiate this ID (including this very next
        // time, at the end of this function) we won't have to parse
        // again.
        // NOTE: The logic here matches that in
        // Transliterator::createFromRules().
        if (entry->stringArg.length() == 0) {
            if (entry->u.data == 0) {
                // No idBlock, no data -- this is just an
                // alias for Null
                entry->entryType = Entry::ALIAS;
                entry->stringArg = NullTransliterator::ID;
            } else {
                // No idBlock, data != 0 -- this is an
                // ordinary RBT_DATA
                entry->entryType = Entry::RBT_DATA;
            }
        } else {
            if (entry->u.data == 0) {
                // idBlock, no data -- this is an alias.  The ID has
                // been munged from reverse into forward mode, if
                // necessary, so instantiate the ID in the forward
                // direction.
                entry->entryType = Entry::ALIAS;
            } else {
                // idBlock and data -- this is a compound
                // RBT
                entry->entryType = Entry::COMPOUND_RBT;
            }
        }
    }

    Transliterator *t =
        instantiateEntry(ID, entry, aliasReturn, parseError,status);
    return t;
}

void TransliteratorRegistry::put(Transliterator* adoptedProto,
                                 UBool visible) {
    Entry *entry = new Entry();
    entry->adoptPrototype(adoptedProto);
    registerEntry(adoptedProto->getID(), entry, visible);
}

void TransliteratorRegistry::put(const UnicodeString& ID,
                                 Transliterator::Factory factory,
                                 Transliterator::Token context,
                                 UBool visible) {
    Entry *entry = new Entry();
    entry->setFactory(factory, context);
    registerEntry(ID, entry, visible);
}

void TransliteratorRegistry::put(const UnicodeString& ID,
                                 const UnicodeString& resourceName,
                                 UTransDirection dir,
                                 UBool visible) {
    Entry *entry = new Entry();
    entry->entryType = (dir == UTRANS_FORWARD) ? Entry::RULES_FORWARD
        : Entry::RULES_REVERSE;
    entry->stringArg = resourceName;
    registerEntry(ID, entry, visible);
}

void TransliteratorRegistry::put(const UnicodeString& ID,
                                 const UnicodeString& alias,
                                 UBool visible) {
    Entry *entry = new Entry();
    entry->entryType = Entry::ALIAS;
    entry->stringArg = alias;
    registerEntry(ID, entry, visible);
}

void TransliteratorRegistry::remove(const UnicodeString& ID) {
    UnicodeString source, target, variant;
    UBool sawSource;
    TransliteratorIDParser::IDtoSTV(ID, source, target, variant, sawSource);
    // Only need to do this if ID.indexOf('-') < 0
    UnicodeString id;
    TransliteratorIDParser::STVtoID(source, target, variant, id);
    registry.remove(id);
    removeSTV(source, target, variant);
    availableIDs.removeElement((void*) &id);
}

//----------------------------------------------------------------------
// class TransliteratorRegistry: Public ID and spec management
//----------------------------------------------------------------------

/**
 * == OBSOLETE - remove in ICU 3.4 ==
 * Return the number of IDs currently registered with the system.
 * To retrieve the actual IDs, call getAvailableID(i) with
 * i from 0 to countAvailableIDs() - 1.
 */
int32_t TransliteratorRegistry::countAvailableIDs(void) const {
    return availableIDs.size();
}

/**
 * == OBSOLETE - remove in ICU 3.4 ==
 * Return the index-th available ID.  index must be between 0
 * and countAvailableIDs() - 1, inclusive.  If index is out of
 * range, the result of getAvailableID(0) is returned.
 */
const UnicodeString& TransliteratorRegistry::getAvailableID(int32_t index) const {
    if (index < 0 || index >= availableIDs.size()) {
        index = 0;
    }
    return *(const UnicodeString*) availableIDs[index];
}

StringEnumeration* TransliteratorRegistry::getAvailableIDs() const {
    return new Enumeration(*this);
}

int32_t TransliteratorRegistry::countAvailableSources(void) const {
    return specDAG.count();
}

UnicodeString& TransliteratorRegistry::getAvailableSource(int32_t index,
                                                          UnicodeString& result) const {
    int32_t pos = -1;
    const UHashElement *e = 0;
    while (index-- >= 0) {
        e = specDAG.nextElement(pos);
        if (e == 0) {
            break;
        }
    }
    if (e == 0) {
        result.truncate(0);
    } else {
        result = *(UnicodeString*) e->key.pointer;
    }
    return result;
}

int32_t TransliteratorRegistry::countAvailableTargets(const UnicodeString& source) const {
    Hashtable *targets = (Hashtable*) specDAG.get(source);
    return (targets == 0) ? 0 : targets->count();
}

UnicodeString& TransliteratorRegistry::getAvailableTarget(int32_t index,
                                                          const UnicodeString& source,
                                                          UnicodeString& result) const {
    Hashtable *targets = (Hashtable*) specDAG.get(source);
    if (targets == 0) {
        result.truncate(0); // invalid source
        return result;
    }
    int32_t pos = -1;
    const UHashElement *e = 0;
    while (index-- >= 0) {
        e = targets->nextElement(pos);
        if (e == 0) {
            break;
        }
    }
    if (e == 0) {
        result.truncate(0); // invalid index
    } else {
        result = *(UnicodeString*) e->key.pointer;
    }
    return result;
}

int32_t TransliteratorRegistry::countAvailableVariants(const UnicodeString& source,
                                                       const UnicodeString& target) const {
    Hashtable *targets = (Hashtable*) specDAG.get(source);
    if (targets == 0) {
        return 0;
    }
    UVector *variants = (UVector*) targets->get(target);
    // variants may be 0 if the source/target are invalid
    return (variants == 0) ? 0 : variants->size();
}

UnicodeString& TransliteratorRegistry::getAvailableVariant(int32_t index,
                                                           const UnicodeString& source,
                                                           const UnicodeString& target,
                                                           UnicodeString& result) const {
    Hashtable *targets = (Hashtable*) specDAG.get(source);
    if (targets == 0) {
        result.truncate(0); // invalid source
        return result;
    }
    UVector *variants = (UVector*) targets->get(target);
    if (variants == 0) {
        result.truncate(0); // invalid target
        return result;
    }
    UnicodeString *v = (UnicodeString*) variants->elementAt(index);
    if (v == 0) {
        result.truncate(0); // invalid index
    } else {
        result = *v;
    }
    return result;
}

//----------------------------------------------------------------------
// class TransliteratorRegistry::Enumeration
//----------------------------------------------------------------------

TransliteratorRegistry::Enumeration::Enumeration(const TransliteratorRegistry& _reg) :
    index(0), reg(_reg) {
}

TransliteratorRegistry::Enumeration::~Enumeration() {
}

int32_t TransliteratorRegistry::Enumeration::count(UErrorCode& /*status*/) const {
    return reg.availableIDs.size();
}

const UnicodeString* TransliteratorRegistry::Enumeration::snext(UErrorCode& status) {
    // This is sloppy but safe -- if we get out of sync with the underlying
    // registry, we will still return legal strings, but they might not
    // correspond to the snapshot at construction time.  So there could be
    // duplicate IDs or omitted IDs if insertions or deletions occur in one
    // thread while another is iterating.  To be more rigorous, add a timestamp,
    // which is incremented with any modification, and validate this iterator
    // against the timestamp at construction time.  This probably isn't worth
    // doing as long as there is some possibility of removing this code in favor
    // of some new code based on Doug's service framework.
    if (U_FAILURE(status)) {
        return NULL;
    }
    int32_t n = reg.availableIDs.size();
    if (index > n) {
        status = U_ENUM_OUT_OF_SYNC_ERROR;
    }
    // index == n is okay -- this means we've reached the end
    if (index < n) {
        // Copy the string! This avoids lifetime problems.
        unistr = *(const UnicodeString*)reg.availableIDs[index++];
        return &unistr;
    } else {
        return NULL;
    }
}

void TransliteratorRegistry::Enumeration::reset(UErrorCode& /*status*/) {
    index = 0;
}

UOBJECT_DEFINE_RTTI_IMPLEMENTATION(TransliteratorRegistry::Enumeration)

//----------------------------------------------------------------------
// class TransliteratorRegistry: internal
//----------------------------------------------------------------------

/**
 * Convenience method.  Calls 6-arg registerEntry().
 */
void TransliteratorRegistry::registerEntry(const UnicodeString& source,
                                           const UnicodeString& target,
                                           const UnicodeString& variant,
                                           Entry* adopted,
                                           UBool visible) {
    UnicodeString ID;
    UnicodeString s(source);
    if (s.length() == 0) {
        s = ANY;
    }
    TransliteratorIDParser::STVtoID(source, target, variant, ID);
    registerEntry(ID, s, target, variant, adopted, visible);
}

/**
 * Convenience method.  Calls 6-arg registerEntry().
 */
void TransliteratorRegistry::registerEntry(const UnicodeString& ID,
                                           Entry* adopted,
                                           UBool visible) {
    UnicodeString source, target, variant;
    UBool sawSource;
    TransliteratorIDParser::IDtoSTV(ID, source, target, variant, sawSource);
    // Only need to do this if ID.indexOf('-') < 0
    UnicodeString id;
    TransliteratorIDParser::STVtoID(source, target, variant, id);
    registerEntry(id, source, target, variant, adopted, visible);
}

/**
 * Register an entry object (adopted) with the given ID, source,
 * target, and variant strings.
 */
void TransliteratorRegistry::registerEntry(const UnicodeString& ID,
                                           const UnicodeString& source,
                                           const UnicodeString& target,
                                           const UnicodeString& variant,
                                           Entry* adopted,
                                           UBool visible) {
    UErrorCode status = U_ZERO_ERROR;
    registry.put(ID, adopted, status);
    if (visible) {
        registerSTV(source, target, variant);
        if (!availableIDs.contains((void*) &ID)) {
            UnicodeString *newID = (UnicodeString *)ID.clone();
            // NUL-terminate the ID string
            newID->getTerminatedBuffer();
            availableIDs.addElement(newID, status);
        }
    } else {
        removeSTV(source, target, variant);
        availableIDs.removeElement((void*) &ID);
    }
}

/**
 * Register a source-target/variant in the specDAG.  Variant may be
 * empty, but source and target must not be.  If variant is empty then
 * the special variant NO_VARIANT is stored in slot zero of the
 * UVector of variants.
 */
void TransliteratorRegistry::registerSTV(const UnicodeString& source,
                                         const UnicodeString& target,
                                         const UnicodeString& variant) {
    // assert(source.length() > 0);
    // assert(target.length() > 0);
    UErrorCode status = U_ZERO_ERROR;
    Hashtable *targets = (Hashtable*) specDAG.get(source);
    if (targets == 0) {
        targets = new Hashtable(TRUE);
        if (targets == 0) {
            return;
        }
        targets->setValueDeleter(uhash_deleteUVector);
        specDAG.put(source, targets, status);
    }
    UVector *variants = (UVector*) targets->get(target);
    if (variants == 0) {
        variants = new UVector(uhash_deleteUnicodeString,
                               uhash_compareCaselessUnicodeString, status);
        if (variants == 0) {
            return;
        }
        targets->put(target, variants, status);
    }
    // assert(NO_VARIANT == "");
    // We add the variant string.  If it is the special "no variant"
    // string, that is, the empty string, we add it at position zero.
    if (!variants->contains((void*) &variant)) {
        if (variant.length() > 0) {
            variants->addElement(new UnicodeString(variant), status);
        } else {
            variants->insertElementAt(new UnicodeString(NO_VARIANT), 0, status);
        }
    }
}

/**
 * Remove a source-target/variant from the specDAG.
 */
void TransliteratorRegistry::removeSTV(const UnicodeString& source,
                                       const UnicodeString& target,
                                       const UnicodeString& variant) {
    // assert(source.length() > 0);
    // assert(target.length() > 0);
//    UErrorCode status = U_ZERO_ERROR;
    Hashtable *targets = (Hashtable*) specDAG.get(source);
    if (targets == 0) {
        return; // should never happen for valid s-t/v
    }
    UVector *variants = (UVector*) targets->get(target);
    if (variants == 0) {
        return; // should never happen for valid s-t/v
    }
    variants->removeElement((void*) &variant);
    if (variants->size() == 0) {
        targets->remove(target); // should delete variants
        if (targets->count() == 0) {
            specDAG.remove(source); // should delete targets
        }
    }
}

/**
 * Attempt to find a source-target/variant in the dynamic registry
 * store.  Return 0 on failure.
 *
 * Caller does NOT own returned object.
 */
Entry* TransliteratorRegistry::findInDynamicStore(const Spec& src,
                                                  const Spec& trg,
                                                  const UnicodeString& variant) const {
    UnicodeString ID;
    TransliteratorIDParser::STVtoID(src, trg, variant, ID);
    Entry *e = (Entry*) registry.get(ID);
    DEBUG_useEntry(e);
    return e;
}

/**
 * Attempt to find a source-target/variant in the static locale
 * resource store.  Do not perform fallback.  Return 0 on failure.
 *
 * On success, create a new entry object, register it in the dynamic
 * store, and return a pointer to it, but do not make it public --
 * just because someone requested something, we do not expand the
 * available ID list (or spec DAG).
 *
 * Caller does NOT own returned object.
 */
Entry* TransliteratorRegistry::findInStaticStore(const Spec& src,
                                                 const Spec& trg,
                                                 const UnicodeString& variant) {
    Entry* entry = 0;
    if (src.isLocale()) {
        entry = findInBundle(src, trg, variant, UTRANS_FORWARD);
    } else if (trg.isLocale()) {
        entry = findInBundle(trg, src, variant, UTRANS_REVERSE);
    }

    // If we found an entry, store it in the Hashtable for next
    // time.
    if (entry != 0) {
        registerEntry(src.getTop(), trg.getTop(), variant, entry, FALSE);
    }

    return entry;
}

// As of 2.0, resource bundle keys cannot contain '_'
static const UChar TRANSLITERATE_TO[] = {84,114,97,110,115,108,105,116,101,114,97,116,101,84,111,0}; // "TransliterateTo"

static const UChar TRANSLITERATE_FROM[] = {84,114,97,110,115,108,105,116,101,114,97,116,101,70,114,111,109,0}; // "TransliterateFrom"

static const UChar TRANSLITERATE[] = {84,114,97,110,115,108,105,116,101,114,97,116,101,0}; // "Transliterate"

/**
 * Attempt to find an entry in a single resource bundle.  This is
 * a one-sided lookup.  findInStaticStore() performs up to two such
 * lookups, one for the source, and one for the target.
 *
 * Do not perform fallback.  Return 0 on failure.
 *
 * On success, create a new Entry object, populate it, and return it.
 * The caller owns the returned object.
 */
Entry* TransliteratorRegistry::findInBundle(const Spec& specToOpen,
                                            const Spec& specToFind,
                                            const UnicodeString& variant,
                                            UTransDirection direction)
{
    UnicodeString utag;
    UnicodeString resStr;
    int32_t pass;

    for (pass=0; pass<2; ++pass) {
        utag.truncate(0);
        // First try either TransliteratorTo_xxx or
        // TransliterateFrom_xxx, then try the bidirectional
        // Transliterate_xxx.  This precedence order is arbitrary
        // but must be consistent and documented.
        if (pass == 0) {
            utag.append(direction == UTRANS_FORWARD ?
                        TRANSLITERATE_TO : TRANSLITERATE_FROM);
        } else {
            utag.append(TRANSLITERATE);
        }
        UnicodeString s(specToFind.get());
        utag.append(s.toUpper());
        CharString tag(utag);
        
        UErrorCode status = U_ZERO_ERROR;
        ResourceBundle subres(specToOpen.getBundle().get(tag, status));
        if (U_FAILURE(status) || status == U_USING_DEFAULT_WARNING) {
            continue;
        }
        
        if (specToOpen.get() != subres.getLocale().getName()) {
            continue;
        }
        
        if (variant.length() != 0) {
            CharString var(variant);
            status = U_ZERO_ERROR;
            resStr = subres.getStringEx(var, status);
            if (U_SUCCESS(status)) {
                // Exit loop successfully
                break;
            }
        }
        
        else {
            // Variant is empty, which means match the first variant listed.
            status = U_ZERO_ERROR;
            resStr = subres.getStringEx(1, status);
            if (U_SUCCESS(status)) {
                // Exit loop successfully
                break;
            }
        }
    }

    if (pass==2) {
        // Failed
        return NULL;
    }

    // We have succeeded in loading a string from the locale
    // resources.  Create a new registry entry to hold it and return it.
    Entry *entry = new Entry();
    if (entry != 0) {
        // The direction is always forward for the
        // TransliterateTo_xxx and TransliterateFrom_xxx
        // items; those are unidirectional forward rules.
        // For the bidirectional Transliterate_xxx items,
        // the direction is the value passed in to this
        // function.
        int32_t dir = (pass == 0) ? UTRANS_FORWARD : direction;
        entry->entryType = Entry::LOCALE_RULES;
        entry->stringArg = resStr;
        entry->intArg = dir;
    }

    return entry;
}

/**
 * Convenience method.  Calls 3-arg find().
 */
Entry* TransliteratorRegistry::find(const UnicodeString& ID) {
    UnicodeString source, target, variant;
    UBool sawSource;
    TransliteratorIDParser::IDtoSTV(ID, source, target, variant, sawSource);
    return find(source, target, variant);
}

/**
 * Top-level find method.  Attempt to find a source-target/variant in
 * either the dynamic or the static (locale resource) store.  Perform
 * fallback.
 * 
 * Lookup sequence for ss_SS_SSS-tt_TT_TTT/v:
 *
 *   ss_SS_SSS-tt_TT_TTT/v -- in hashtable
 *   ss_SS_SSS-tt_TT_TTT/v -- in ss_SS_SSS (no fallback)
 * 
 *     repeat with t = tt_TT_TTT, tt_TT, tt, and tscript
 *
 *     ss_SS_SSS-t/ *
 *     ss_SS-t/ *
 *     ss-t/ *
 *     sscript-t/ *
 *
 * Here * matches the first variant listed.
 *
 * Caller does NOT own returned object.  Return 0 on failure.
 */
Entry* TransliteratorRegistry::find(UnicodeString& source,
                                    UnicodeString& target,
                                    UnicodeString& variant) {
    
    Spec src(source);
    Spec trg(target);
    Entry* entry;

    if (variant.length() != 0) {
        
        // Seek exact match in hashtable
        entry = findInDynamicStore(src, trg, variant);
        if (entry != 0) {
            return entry;
        }
        
        // Seek exact match in locale resources
        entry = findInStaticStore(src, trg, variant);
        if (entry != 0) {
            return entry;
        }
    }

    for (;;) {
        src.reset();
        for (;;) {
            // Seek match in hashtable
            entry = findInDynamicStore(src, trg, NO_VARIANT);
            if (entry != 0) {
                return entry;
            }
            
            // Seek match in locale resources
            entry = findInStaticStore(src, trg, NO_VARIANT);
            if (entry != 0) {
                return entry;
            }
            if (!src.hasFallback()) {
                break;
            }
            src.next();
        }
        if (!trg.hasFallback()) {
            break;
        }
        trg.next();
    }

    return 0;
}

/**
 * Given an Entry object, instantiate it.  Caller owns result.  Return
 * 0 on failure.
 *
 * Return a non-empty aliasReturn value if the ID points to an alias.
 * We cannot instantiate it ourselves because the alias may contain
 * filters or compounds, which we do not understand.  Caller should
 * make aliasReturn empty before calling.
 *
 * The entry object is assumed to reside in the dynamic store.  It may be
 * modified.
 */
Transliterator* TransliteratorRegistry::instantiateEntry(const UnicodeString& ID,
                                                         Entry *entry,
                                                         TransliteratorAlias* &aliasReturn,
                                                         UParseError& parseError,
                                                         UErrorCode& status) {
    Transliterator *t = 0;
    U_ASSERT(aliasReturn == 0);

    switch (entry->entryType) {
    case Entry::RBT_DATA:
        t = new RuleBasedTransliterator(ID, entry->u.data);
        if (t == 0) {
            status = U_MEMORY_ALLOCATION_ERROR;
        }
        return t;
    case Entry::PROTOTYPE:
        t = entry->u.prototype->clone();
        if (t == 0) {
            status = U_MEMORY_ALLOCATION_ERROR;
        }
        return t;
    case Entry::ALIAS:
        aliasReturn = new TransliteratorAlias(entry->stringArg);
        if (aliasReturn == 0) {
            status = U_MEMORY_ALLOCATION_ERROR;
        }
        return 0;
    case Entry::FACTORY:
        t = entry->u.factory.function(ID, entry->u.factory.context);
        if (t == 0) {
            status = U_MEMORY_ALLOCATION_ERROR;
        }
        return t;
    case Entry::COMPOUND_RBT:
        {
            UnicodeString id("_", "");
            Transliterator *t = new RuleBasedTransliterator(id, entry->u.data);
            if (t == 0) {
                status = U_MEMORY_ALLOCATION_ERROR;
                return 0;
            }
            aliasReturn = new TransliteratorAlias(ID, entry->stringArg, t, entry->intArg, entry->compoundFilter);
        }
        if (aliasReturn == 0) {
            status = U_MEMORY_ALLOCATION_ERROR;
        }
        return 0;
    case Entry::LOCALE_RULES:
        aliasReturn = new TransliteratorAlias(ID, entry->stringArg,
                                              (UTransDirection) entry->intArg);
        if (aliasReturn == 0) {
            status = U_MEMORY_ALLOCATION_ERROR;
        }
        return 0;
    case Entry::RULES_FORWARD:
    case Entry::RULES_REVERSE:
        // Process the rule data into a TransliteratorRuleData object,
        // and possibly also into an ::id header and/or footer.  Then
        // we modify the registry with the parsed data and retry.
        {
            TransliteratorParser parser;
            
            // We use the file name, taken from another resource bundle
            // 2-d array at static init time, as a locale language.  We're
            // just using the locale mechanism to map through to a file
            // name; this in no way represents an actual locale.
            CharString ch(entry->stringArg);
            UResourceBundle *bundle = ures_openDirect(0, ch, &status);
            UnicodeString rules = ures_getUnicodeStringByKey(bundle, RB_RULE, &status);
            ures_close(bundle);
            
            if (U_FAILURE(status)) {
                // We have a failure of some kind.  Remove the ID from the
                // registry so we don't keep trying.  NOTE: This will throw off
                // anyone who is, at the moment, trying to iterate over the
                // available IDs.  That's acceptable since we should never
                // really get here except under installation, configuration,
                // or unrecoverable run time memory failures.
                remove(ID);
            } else {
                
                // If the status indicates a failure, then we don't have any
                // rules -- there is probably an installation error.  The list
                // in the root locale should correspond to all the installed
                // transliterators; if it lists something that's not
                // installed, we'll get an error from ResourceBundle.
                aliasReturn = new TransliteratorAlias(ID, rules,
                    ((entry->entryType == Entry::RULES_REVERSE) ?
                     UTRANS_REVERSE : UTRANS_FORWARD));
                if (aliasReturn == 0) {
                    status = U_MEMORY_ALLOCATION_ERROR;
                }
            }
        }
        return 0;
    default:
        U_ASSERT(FALSE); // can't get here
        return 0;
    }
}
U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_TRANSLITERATION */

//eof