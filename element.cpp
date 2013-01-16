
/*
 * Copyright David Wilson, 2013.
 * License: http://opensource.org/licenses/MIT
 */

#include <cassert>
#include <cstring>

#include <unistd.h>

#include <fstream>

#include <libxml/parser.h>
#include <libxml/xmlsave.h>

#include "element.hpp"


namespace etree {

using std::string;
using std::ostream;


/// -----------------------
/// Nullable implementation
/// -----------------------

template<typename T>
Nullable<T>::Nullable()
    : set_(false)
{
}


template<typename T>
Nullable<T>::Nullable(const T &val)
    : set_(true)
{
    new (reinterpret_cast<T *>(val_)) T(val);
}


template<typename T>
Nullable<T>::Nullable(const Nullable<T> &val)
    : set_(val.set_)
{
    if(set_) {
        new (reinterpret_cast<T *>(val_)) T(*val);
    }
}


#ifdef ETREE_0X
template<typename T>
Nullable<T>::Nullable(T &&val)
    : set_(true)
{
    new (reinterpret_cast<T *>(val_)) T(val);
}
#endif


template<typename T>
Nullable<T>::~Nullable()
{
    if(set_) {
        T &val = *reinterpret_cast<T *>(val_);
        val.~T();
    }
}


template<typename T>
Nullable<T>::operator bool() const
{
    return set_;
}


template<typename T>
T &Nullable<T>::operator *()
{
    if(! set_) {
        throw missing_value_error();
    }
    return *reinterpret_cast<T *>(val_);
}


template<typename T>
const T &Nullable<T>::operator *() const
{
    if(! set_) {
        throw missing_value_error();
    }
    return *reinterpret_cast<const T *>(val_);
}


// Instantiations.
template class Nullable<Element>;


/// ----------------------------------------
/// libxml2 DOM reference counting functions
/// ----------------------------------------

static xmlDocPtr ref(xmlDocPtr doc)
{
    // Relies on NULL (aka. initial state of _private) being (intptr_t)0, which
    // isn't true on some weird archs.
    assert(doc && (sizeof(void *) >= sizeof(intptr_t)));
    (*reinterpret_cast<intptr_t *>(&(doc->_private)))++;
    return doc;
}

static void unref(xmlDocPtr doc)
{
    assert(doc && (sizeof(void *) >= sizeof(intptr_t)));
    assert(doc->_private);
    if(! --*reinterpret_cast<intptr_t *>(&(doc->_private))) {
        xmlFreeDoc(doc);
    }
}

static xmlNodePtr ref(xmlNodePtr node)
{
    assert(node);
    if(! (*reinterpret_cast<intptr_t *>(&(node->_private)))++) {
        ref(node->doc);
    }
    return node;
}

static void unref(xmlNodePtr node)
{
    assert(node);
    assert(node->_private);
    if(! --*reinterpret_cast<intptr_t *>(&(node->_private))) {
        unref(node->doc);
    }
}


/// -------------------------
/// Internal helper functions
/// -------------------------


#ifdef ETREE_0X
static void attrs_from_list_(Element &elem, kv_list &attribs)
{
    std::cout << "LOL!" << std::endl;
    AttrMap attrs = elem.attrib();
    for(auto &kv : attribs) {
        std::cout << "APPEND: " << kv.first << " V " << kv.second << "\n" << std::endl;
        attrs.set(kv.first, kv.second);
    }
}
#endif


static const xmlChar *c_str(const string &s)
{
    return (const xmlChar *) (s.empty() ? 0 : s.c_str());
}


static xmlNodePtr _textNodeOrSkip(xmlNodePtr node)
{
    while(node) {
        switch(node->type) {
            case XML_TEXT_NODE:
            case XML_CDATA_SECTION_NODE:
                return node;
            case XML_XINCLUDE_START:
            case XML_XINCLUDE_END:
                node = node->next;
                break;
            default:
                return 0;
        }
    }
    return 0;
}


static void _removeText(xmlNodePtr node)
{
    node = _textNodeOrSkip(node);
    while(node) {
        xmlNodePtr next = _textNodeOrSkip(node);
        ::xmlUnlinkNode(node);
        ::xmlFreeNode(node);
        node = next;
    }
}


static void _setNodeText(xmlNodePtr node, const string &s)
{
    _removeText(node->children);
    // TODO: CDATA
    if(s.size()) {
        xmlNodePtr text = ::xmlNewText((xmlChar *) s.c_str());
        assert(text != 0);
        if(node->children) {
            ::xmlAddPrevSibling(node->children, text);
        } else {
            ::xmlAddChild(node, text);
        }
    }
}


static void _setTailText(xmlNodePtr node, const string &s)
{
    _removeText(node->next);
    if(s.size()) {
        xmlNodePtr text = ::xmlNewText((xmlChar *) s.c_str());
        assert(text != 0);
        ::xmlAddNextSibling(node, text);
    }
}


static string _collectText(xmlNodePtr node)
{
    string result;
    while(node) {
        result += (const char *) node->content;
        node = _textNodeOrSkip(node);
    }
    return result;
}


static const xmlNsPtr findNs_(xmlNodePtr node, const string &uri,
                              bool create=false)
{
    if(uri.empty()) {
        return 0; // TODO: return default namespace
    }

    for(xmlNodePtr cur = node; cur != 0; cur = cur->parent) {
        for(xmlNsPtr ns = node->nsDef; ns != 0; ns = ns->next) {
            if(uri == reinterpret_cast<const char *>(ns->href)) {
                return ns;
            }
        }
    }

    throw missing_namespace_error();
}


template<typename P, typename T>
P nodeFor__(const T &e)
{
    return e.node_;
}


/// ---------------
/// QName functions
/// ---------------


void QName::from_string(const string &qname)
{
    if(qname.size() > 0 && qname[0] == '{') {
        size_t e = qname.find('}');
        if(e == string::npos) {
            throw qname_error();
        } else if(qname.size() - 1 == e) {
            throw qname_error();
        }
        ns_ = qname.substr(1, e - 1);
        tag_ = qname.substr(e + 1);
        if(tag_.size() == 0) {
            throw qname_error();
        }
    } else {
        ns_ = "";
        tag_ = qname;
    }
}


string QName::tostring() const
{
    if(ns_.empty()) {
        return tag_;
    }

    string qname(2 + ns_.size() + tag_.size(), '\0');
    int p = 0;
    qname[p++] = '{';
    qname.replace(p, ns_.size(), ns_);
    qname[p += ns_.size()] = '}';
    qname.replace(p + 1, tag_.size(), tag_);
    return qname;
}


QName::QName(const string &ns, const string &tag)
    : ns_(ns)
    , tag_(tag)
{
}


QName::QName(const QName &other)
    : ns_(other.ns_)
    , tag_(other.tag_)
{
}


QName::QName(const string &qname)
{
    from_string(qname);
}


QName::QName(const char *qname)
{
    from_string(qname);
}


const string &QName::tag() const
{
    return tag_;
}


const string &QName::ns() const
{
    return ns_;
}


bool QName::operator=(const QName &other)
{
    return other.tag_ == tag_ && other.ns_ == ns_;
}


/// -----------------
/// AttrMap iterators
/// -----------------

AttrIterator::AttrIterator(xmlNodePtr node)
    : node_(node)
{
}


QName AttrIterator::key()
{
    const char *ns = "";
    if(node_->nsDef) {
        ns = reinterpret_cast<const char *>(node_->nsDef->href);
    }
    return QName(ns, reinterpret_cast<const char *>(node_->name));
}


string AttrIterator::value()
{
    string out;
    xmlChar *s = ::xmlNodeListGetString(node_->doc,
                                        node_->children, 1);
    if(s) {
        out = reinterpret_cast<const char *>(s);
        ::xmlFree(s);
    }

    return out;
}


bool AttrIterator::next()
{
    if(node_->next) {
        node_ = ref(node_->next);
        unref(node_->prev);
        return true;
    }
    return false;
}


/// -----------------
/// AttrMap functions
/// -----------------

AttrMap::AttrMap(xmlNodePtr node)
    : node_(ref(node))
{
}


AttrMap::~AttrMap()
{
    unref(node_);
}


bool AttrMap::has(const QName &qname) const
{
    return ::xmlHasNsProp(node_, c_str(qname.tag()), c_str(qname.ns()));
}


string AttrMap::get(const QName &qname,
                         const string &default_) const
{
    string out(default_);
    xmlChar *s = ::xmlGetNsProp(node_, c_str(qname.tag()), c_str(qname.ns()));

    if(s) {
        out = reinterpret_cast<const char *>(s);
        ::xmlFree(reinterpret_cast<void *>(s));
    }
    return out;
}


void AttrMap::set(const QName &qname, const string &s)
{
    ::xmlSetNsProp(node_,
       findNs_(node_, qname.ns()), c_str(qname.tag()), c_str(s));
}


std::vector<QName> AttrMap::keys() const
{
    std::vector<QName> names;
    xmlAttrPtr p = node_->properties;
    while(p) {
        const char *ns = p->ns ? (const char *)p->ns->href : "";
        names.push_back(QName(ns, (const char *)p->name));
        p = p->next;
    }
    return names;
}


/// ---------------------
/// ElementTree functions
/// ---------------------

ElementTree::~ElementTree()
{
    unref(node_);
}


ElementTree::ElementTree(xmlDocPtr doc)
    : node_(ref(doc))
{
}


/// -----------------
/// Element functions
/// -----------------

Element::~Element()
{
    unref(node_);
}


Element::Element(const Element &e)
    : node_(ref(e.node_))
{
}


Element::Element(xmlNodePtr node)
    : node_(ref(node))
{
}


static xmlNodePtr node_from_qname(const QName &qname)
{
    xmlDocPtr doc = ::xmlNewDoc(0);
    if(! doc) {
        throw memory_error();
    }

    xmlNodePtr node = ::xmlNewDocNode(doc, 0,
        reinterpret_cast<const xmlChar *>(qname.tag().c_str()), 0);
    if(! node) {
        ::xmlFreeDoc(doc);
        throw memory_error();
    }

    ::xmlDocSetRootElement(doc, node);
    ref(node);

    if(qname.ns().size()) {
        xmlNsPtr ns = ::xmlNewNs(node, (xmlChar *)qname.ns().c_str(), 0);
        if(ns == 0) {
            unref(node);
            throw memory_error();
        }
        ::xmlSetNs(node, ns);
    }

    return node;
}


Element::Element(const QName &qname)
    : node_(node_from_qname(qname))
{
}


#ifdef ETREE_0X
Element::Element(const QName &qname, kv_list attribs)
    : node_(node_from_qname(qname))
{
    try {
        attrs_from_list_(*this, attribs);
    } catch(...) {
        unref(node_);
        throw;
    }
}
#endif


size_t Element::size() const
{
    return ::xmlChildElementCount(node_);
}


QName Element::qname() const
{
    return QName(ns(), tag());
}


void Element::qname(const QName &qname)
{
    ns(qname.ns());
    tag(qname.tag());
}


string Element::tag() const
{
    return reinterpret_cast<const char *>(node_->name);
}


void Element::tag(const string &s)
{
    ::xmlNodeSetName(node_, c_str(s));
}


string Element::ns() const
{
    if(node_->nsDef) {
        return reinterpret_cast<const char *>(node_->nsDef->href);
    }
    return "";
}


void Element::ns(const string &ns)
{
    if(ns.empty()) {
        node_->ns = 0;
    } else {
        
    }
}


AttrMap Element::attrib() const
{
    return AttrMap(node_);
}


string Element::get(const QName &qname, const string &default_) const
{
    return attrib().get(qname, default_);
}


Element Element::operator[] (size_t i)
{
    xmlNodePtr cur = node_->children;
    while(i) {
        if(cur->type == XML_ELEMENT_NODE) {
            i--;
        }
        cur = cur->next;
        if(! cur) {
            throw out_of_bounds_error();
        }
    }
    return Element(cur);
}


bool Element::isIndirectParent(const Element &e)
{
    xmlNodePtr other = e.node_;
    xmlNodePtr parent = node_->parent;
    while(parent) {
        if(parent == other) {
            return true;
        }
        parent = parent->parent;
    }
    return false;
}


void Element::append(Element &e)
{
    std::cout << "appending " << e;
    std::cout << " to " << *this << std::endl;
    if(isIndirectParent(e)) {
        throw cyclical_tree_error();
    }
    ::xmlUnlinkNode(e.node_);
    ::xmlAddChild(node_, e.node_);
}


void Element::insert(size_t i, Element &e)
{
    if(i == size()) {
        append(e);
    } else {
        ::xmlAddPrevSibling(this[i].node_, e.node_);
    }
}


void Element::remove(Element &e)
{
    if(e.node_->parent == node_) {
        ::xmlUnlinkNode(e.node_);
    }
}


Nullable<Element> Element::getnext() const
{
    if(node_->next) {
        return Nullable<Element>(ref(node_->next));
    }
    return Nullable<Element>();
}


Nullable<Element> Element::getparent() const
{
    xmlNodePtr parent = node_->parent;
    if(parent && parent->type != XML_DOCUMENT_NODE) {
        return Nullable<Element>(ref(parent));
    }
    return Nullable<Element>();
}


ElementTree Element::getroottree() const
{
    return ElementTree(node_->doc);
}


string Element::text() const
{
    return _collectText(node_->children);
}


void Element::text(const string &s)
{
    _setNodeText(node_, s);
}


string Element::tail() const
{
    return _collectText(node_->next);
}


void Element::tail(const string &s)
{
    _setTailText(node_, s);
}


/// -------------------------
/// tostring() implementation
/// -------------------------


static int writeCallback(void *ctx, const char *buffer, int len)
{
    string *s = static_cast<string *>(ctx);
    s->append(buffer, len);
    return len;
}


static int closeCallback(void *ctx)
{
    return 0;
}


string tostring(const Element &e)
{
    string out;
    xmlSaveCtxtPtr ctx = ::xmlSaveToIO(writeCallback, closeCallback,
        static_cast<void *>(&out), 0, 0);

    int ret = ::xmlSaveTree(ctx, nodeFor__<xmlNodePtr, Element>(e));
    ::xmlSaveClose(ctx);
    if(ret == -1) {
        throw serialization_error();
    }
    return out;
}


/// ----------------
/// Helper functions
/// ----------------

Element SubElement(Element &parent, const QName &qname)
{
    Element elem(qname);
    parent.append(elem);
    return elem;
}


#ifdef ETREE_0X
Element SubElement(Element &parent, const QName &qname, kv_list attribs)
{
    Element elem = SubElement(parent, qname);
    attrs_from_list_(elem, attribs);
    return elem;
}
#endif


static Element fromstring_internal(const char *s, size_t size)
{
    xmlDocPtr doc = ::xmlReadMemory(s, size, 0, 0, 0);
    if(! (doc && doc->children)) {
        ::xmlFreeDoc(doc); // NULL ok.
        throw parse_error();
    }

    return Element(doc->children);
}


Element fromstring(const char *s)
{
    return fromstring_internal(s, ::strlen(s));
}


Element fromstring(const string &s)
{
    return fromstring_internal(s.data(), s.size());
}


Element XML(const char *s)
{
    return fromstring_internal(s, ::strlen(s));
}


Element XML(const string &s)
{
    return fromstring_internal(s.data(), s.size());
}


/// ----------------------
/// parse() implementation
/// ----------------------

static int DUMMY_close(void *ignored)
{
    return 0;
}


int istream_read__(void *strm, char *buffer, int len)
{
    std::istream &is = *static_cast<std::istream *>(strm);

    is.read(buffer, len);
    if(is.fail() && !is.eof()) {
        return -1;
    }
    return is.gcount();
}


int fd_read__(void *strm, char *buffer, int len)
{
    int &fd = *static_cast<int *>(strm);
    return ::read(fd, buffer, len);
}


template<int(*fn)(void *, char *, int), typename T>
static ElementTree parse_internal(T obj)
{
    xmlDocPtr doc = ::xmlReadIO(fn, DUMMY_close,
                                static_cast<void *>(obj), 0, 0, 0);
    if(! doc) {
        throw parse_error();
    }
    return ElementTree(doc);
}


ElementTree parse(std::istream &is)
{
    return parse_internal<istream_read__>(&is);
}


ElementTree parse(const string &path)
{
    std::ifstream is(path.c_str(), std::ios_base::binary);
    return parse(is);
}


ElementTree parse(int fd)
{
    return parse_internal<fd_read__>(&fd);
}


/// -----------------
/// iostreams support
/// -----------------


ostream &operator<< (ostream &out, const ElementTree &tree)
{
    out << "<ElementTree at " << nodeFor__<xmlDocPtr, ElementTree>(tree) << ">";
    return out;
}


ostream &operator<< (ostream &out, const Element &elem)
{
    out << "<Element " << elem.qname().tostring() << " at ";
    out << nodeFor__<xmlNodePtr, Element>(elem);
    out << " with " << elem.size() << " children>";
    return out;
}


ostream& operator<< (ostream& out, const QName& qname)
{
    if(qname.ns().size()) {
        out << "{" << qname.ns() << "}";
    }
    out << qname.tag();
    return out;
}


} // namespace
