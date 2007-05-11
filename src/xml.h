/*
 * Copyright (c) 2003-2007, John Wiegley.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of New Artisans LLC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _XML_H
#define _XML_H

#include "journal.h"
#include "value.h"
#include "parser.h"

namespace ledger {

class transaction_t;
class entry_t;
class account_t;
class journal_t;

namespace xml {

#define XML_NODE_IS_PARENT 0x1

DECLARE_EXCEPTION(conversion_error);

class parent_node_t;
class document_t;

class node_t : public supports_flags<>
{
public:
  typedef uint_fast16_t nameid_t;

  nameid_t	      name_id;
#ifdef THREADSAFE
  document_t *	      document;
#else
  static document_t * document;
#endif
  parent_node_t *     parent;
  node_t *	      next;
  node_t *	      prev;

  typedef std::map<string, string> attrs_map;

  attrs_map * attrs;

  node_t(document_t * _document, parent_node_t * _parent = NULL,
	 flags_t _flags = 0);

  virtual ~node_t() {
    TRACE_DTOR(node_t);
    if (parent) extract();
    if (attrs) checked_delete(attrs);
  }

  parent_node_t * as_parent_node() {
    if (! has_flags(XML_NODE_IS_PARENT))
      throw_(std::logic_error, "Request to cast leaf node to a parent node");
    return polymorphic_downcast<parent_node_t *>(this);
  }    
  const parent_node_t * as_parent_node() const {
    if (! has_flags(XML_NODE_IS_PARENT))
      throw_(std::logic_error, "Request to cast leaf node to a parent node");
    return polymorphic_downcast<const parent_node_t *>(this);
  }    

  void extract();		// extract this node from its parent's child list

  virtual const char * text() const {
    assert(false);
    return NULL;
  }

  const char * name() const;
  int set_name(const char * _name);
  int set_name(int _name_id) {
    name_id = _name_id;
    return name_id;
  }

  void set_attr(const char * n, const char * v) {
    if (! attrs)
      attrs = new attrs_map;
    std::pair<attrs_map::iterator, bool> result =
      attrs->insert(attrs_map::value_type(n, v));
    assert(result.second);
  }
  const char * get_attr(const char * n) {
    if (attrs) {
      attrs_map::iterator i = attrs->find(n);
      if (i != attrs->end())
	return (*i).second.c_str();
    }
    return NULL;
  }

  node_t * lookup_child(const char * _name) const;
  node_t * lookup_child(const string& _name) const;
  virtual node_t * lookup_child(int /* _name_id */) const {
    return NULL;
  }

  virtual value_t to_value() const {
    throw_(conversion_error, "Cannot convert node to a value");
    return value_t();
  }

  virtual void print(std::ostream& out, int depth = 0) const = 0;

private:
  node_t(const node_t&);
  node_t& operator=(const node_t&);
};

class parent_node_t : public node_t
{
public:
  mutable node_t * _children;
  mutable node_t * _last_child;

  parent_node_t(document_t * _document, parent_node_t * _parent = NULL)
    : node_t(_document, _parent, XML_NODE_IS_PARENT),
      _children(NULL), _last_child(NULL)
  {
    TRACE_CTOR(parent_node_t, "document_t *, parent_node_t *");
  }
  virtual ~parent_node_t() {
    TRACE_DTOR(parent_node_t);
    if (_children) clear();
  }

  virtual void	   clear();	// clear out all child nodes
  virtual node_t * children() const {
    return _children;
  }
  virtual node_t * last_child() {
    if (! _children)
      children();
    return _last_child;
  }
  virtual void	   add_child(node_t * node);

  void print(std::ostream& out, int depth = 0) const;

private:
  parent_node_t(const parent_node_t&);
  parent_node_t& operator=(const parent_node_t&);
};

class terminal_node_t : public node_t
{
  string data;

public:
  terminal_node_t(document_t * _document, parent_node_t * _parent = NULL)
    : node_t(_document, _parent)
  {
    TRACE_CTOR(terminal_node_t, "document_t *, parent_node_t *");
  }
  virtual ~terminal_node_t() {
    TRACE_DTOR(terminal_node_t);
  }

  virtual const char * text() const {
    return data.c_str();
  }
  virtual void set_text(const char * _data) {
    data = _data;
  }
  virtual void set_text(const string& _data) {
    data = _data;
  }

  virtual value_t to_value() const {
    return text();
  }

  void print(std::ostream& out, int depth = 0) const;

private:
  terminal_node_t(const node_t&);
  terminal_node_t& operator=(const node_t&);
};

class document_t
{
  static const char * 	   ledger_builtins[];
  static const std::size_t ledger_builtins_size;

public:
  enum ledger_builtins_t {
    ACCOUNT = 10,
    ACCOUNT_PATH,
    AMOUNT,
    CODE,
    COMMODITY,
    ENTRIES,
    ENTRY,
    JOURNAL,
    NAME,
    NOTE,
    PAYEE,
    TRANSACTION
  };

private:
  typedef std::vector<string> names_array;

  names_array names;

  typedef std::map<string, int> names_map;

  names_map names_index;

public:
  node_t * top;

private:
  terminal_node_t stub;

public:
  // Ids 0-9 are reserved.  10-999 are for "builtin" names.  1000+ are
  // for dynamically registered names.
  enum special_names_t {
    CURRENT, PARENT, ROOT, ALL
  };

  document_t(node_t * _top = NULL)
    : top(_top ? _top : &stub), stub(this) {
    TRACE_CTOR(xml::document_t, "node_t *, const char **, const int");
  }
  ~document_t();

  void set_top(node_t * _top);

  int register_name(const string& name);
  int lookup_name_id(const string& name) const;
  static int lookup_builtin_id(const string& name);
  const char * lookup_name(int id) const;

  void print(std::ostream& out) const;

#if defined(HAVE_EXPAT) || defined(HAVE_XMLPARSE)
  class parser_t
  {
  public:
    document_t *	document;
    XML_Parser		parser;
    string		have_error;
    const char *	pending;
    node_t::attrs_map * pending_attrs;
    bool                handled_data;

    std::list<parent_node_t *> node_stack;

    parser_t() : document(NULL), pending(NULL), pending_attrs(NULL),
		 handled_data(false) {}
    virtual ~parser_t() {}

    virtual bool         test(std::istream& in) const;
    virtual document_t * parse(std::istream& in);
  };
#endif
};

#if defined(HAVE_EXPAT) || defined(HAVE_XMLPARSE)

class xml_parser_t : public parser_t
{
 public:
  virtual bool test(std::istream& in) const;

  virtual unsigned int parse(std::istream&	   in,
			     journal_t *	   journal,
			     account_t *	   master   = NULL,
			     const optional<path>& original = optional<path>());
};

DECLARE_EXCEPTION(parse_error);

#endif

class commodity_node_t : public parent_node_t
{
public:
  commodity_t * commodity;

  commodity_node_t(document_t *    _document,
		   commodity_t *   _commodity,
		   parent_node_t * _parent = NULL)
    : parent_node_t(_document, _parent), commodity(_commodity) {
    TRACE_CTOR(commodity_node_t, "document_t *, commodity_t *, parent_node_t *");
    set_name(document_t::COMMODITY);
  }
  virtual ~commodity_node_t() {
    TRACE_DTOR(commodity_node_t);
  }

  virtual node_t * children() const;
};

class amount_node_t : public parent_node_t
{
public:
  amount_t * amount;

  amount_node_t(document_t *    _document,
		amount_t *      _amount,
		parent_node_t * _parent = NULL)
    : parent_node_t(_document, _parent), amount(_amount) {
    TRACE_CTOR(amount_node_t, "document_t *, amount_t *, parent_node_t *");
    set_name(document_t::AMOUNT);
  }
  virtual ~amount_node_t() {
    TRACE_DTOR(amount_node_t);
  }

  virtual node_t * children() const;

  virtual value_t to_value() const {
    return *amount;
  }
};

class transaction_node_t : public parent_node_t
{
  mutable terminal_node_t * payee_virtual_node;

public:
  transaction_t * transaction;

  transaction_node_t(document_t *    _document,
		     transaction_t * _transaction,
		     parent_node_t * _parent = NULL)
    : parent_node_t(_document, _parent), payee_virtual_node(NULL),
      transaction(_transaction) {
    TRACE_CTOR(transaction_node_t, "document_t *, transaction_t *, parent_node_t *");
    set_name(document_t::TRANSACTION);
  }
  virtual ~transaction_node_t() {
    TRACE_DTOR(transaction_node_t);
    if (payee_virtual_node)
      checked_delete(payee_virtual_node);
  }

  virtual node_t * children() const;
  virtual node_t * lookup_child(int _name_id) const;
  virtual value_t  to_value() const;
};

class entry_node_t : public parent_node_t
{
  entry_t *  entry;

public:
  entry_node_t(document_t * _document, entry_t * _entry,
	       parent_node_t * _parent = NULL)
    : parent_node_t(_document, _parent), entry(_entry) {
    TRACE_CTOR(entry_node_t, "document_t *, entry_t *, parent_node_t *");
    set_name(document_t::ENTRY);
  }
  virtual ~entry_node_t() {
    TRACE_DTOR(entry_node_t);
  }

  virtual node_t * children() const;
  virtual node_t * lookup_child(int _name_id) const;

  friend class transaction_node_t;
};

class account_node_t : public parent_node_t
{
  account_t * account;

public:
  account_node_t(document_t * _document, account_t * _account,
		 parent_node_t * _parent = NULL)
    : parent_node_t(_document, _parent), account(_account) {
    TRACE_CTOR(account_node_t, "document_t *, account_t *, parent_node_t *");
    set_name(document_t::ACCOUNT);
  }
  virtual ~account_node_t() {
    TRACE_DTOR(account_node_t);
  }

  virtual node_t * children() const;
};

class journal_node_t : public parent_node_t
{
  journal_t * journal;

public:
  journal_node_t(document_t * _document, journal_t * _journal,
		 parent_node_t * _parent = NULL)
    : parent_node_t(_document, _parent), journal(_journal) {
    TRACE_CTOR(journal_node_t, "document_t *, journal_t *, parent_node_t *");
    set_name(document_t::JOURNAL);
  }
  virtual ~journal_node_t() {
    TRACE_DTOR(journal_node_t);
  }

  virtual node_t * children() const;

  friend class transaction_node_t;
};

template <typename T>
inline typename T::node_type *
wrap_node(document_t * doc, T * item, void * parent_node = NULL) {
  assert(false);
  return NULL;
}

template <>
inline transaction_t::node_type *
wrap_node(document_t * doc, transaction_t * xact, void * parent_node) {
  return new transaction_node_t(doc, xact, (parent_node_t *)parent_node);
}

template <>
inline entry_t::node_type *
wrap_node(document_t * doc, entry_t * entry, void * parent_node) {
  return new entry_node_t(doc, entry, (parent_node_t *)parent_node);
}

template <>
inline account_t::node_type *
wrap_node(document_t * doc, account_t * account, void * parent_node) {
  return new account_node_t(doc, account, (parent_node_t *)parent_node);
}

template <>
inline journal_t::node_type *
wrap_node(document_t * doc, journal_t * journal, void * parent_node) {
  return new journal_node_t(doc, journal, (parent_node_t *)parent_node);
}

} // namespace xml
} // namespace ledger

#endif // _XML_H
