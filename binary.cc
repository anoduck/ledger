#include "ledger.h"
#include "binary.h"

#include <ctime>
#include <sys/stat.h>

#define TIMELOG_SUPPORT 1

namespace ledger {

const unsigned long	   binary_magic_number = 0xFFEED765;
static const unsigned long format_version      = 0x00020019;

static account_t **	   accounts;
static account_t **	   accounts_next;
static unsigned int	   account_index;

static commodity_t **	   commodities;
static commodity_t **	   commodities_next;
static unsigned int	   commodity_index;

amount_t::bigint_t *	   bigints;
amount_t::bigint_t *	   bigints_next;
unsigned int		   bigints_index;
unsigned int		   bigints_count;

#if DEBUG_LEVEL >= ALPHA
#define read_binary_guard(in, id) {		\
  unsigned short guard;				\
  in.read((char *)&guard, sizeof(guard));	\
  assert(guard == id);				\
}
#else
#define read_binary_guard(in, id)
#endif

template <typename T>
inline void read_binary_number(std::istream& in, T& num) {
  in.read((char *)&num, sizeof(num));
}

template <typename T>
inline T read_binary_number(std::istream& in) {
  T num;
  in.read((char *)&num, sizeof(num));
  return num;
}

inline void read_binary_string(std::istream& in, std::string& str)
{
  read_binary_guard(in, 0x3001);

  unsigned char len;
  read_binary_number(in, len);
  if (len == 0xff) {
    unsigned short slen;
    read_binary_number(in, slen);
    char * buf = new char[slen + 1];
    in.read(buf, slen);
    buf[slen] = '\0';
    str = buf;
    delete[] buf;
  }
  else if (len) {
    char buf[256];
    in.read(buf, len);
    buf[len] = '\0';
    str = buf;
  } else {
    str = "";
  }

  read_binary_guard(in, 0x3002);
}

inline std::string read_binary_string(std::istream& in)
{
  std::string temp;
  read_binary_string(in, temp);
  return temp;
}

inline void read_binary_amount(std::istream& in, amount_t& amt)
{
  commodity_t::ident_t ident;
  read_binary_number(in, ident);
  if (ident == 0xffffffff)
    amt.commodity = NULL;
  else
    amt.commodity = commodities[ident - 1];

  amt.read_quantity(in);
}

inline void init_binary_string(char *& string_pool, std::string * str)
{
#if DEBUG_LEVEL >= ALPHA
  unsigned short guard;
  guard = *((unsigned short *) string_pool);
  string_pool += sizeof(unsigned short);
  assert(guard == 0x3001);
#endif

  unsigned char len = *string_pool++;
  if (len == 0xff) {
    unsigned short slen = *((unsigned short *) string_pool);
    new(str) std::string(string_pool + sizeof(unsigned short), slen);
    string_pool += sizeof(unsigned short) + slen;
  }
  else if (len) {
    new(str) std::string(string_pool, len);
    string_pool += len;
  }
  else {
    new(str) std::string("");
  }

#if DEBUG_LEVEL >= ALPHA
  guard = *((unsigned short *) string_pool);
  string_pool += sizeof(unsigned short);
  assert(guard == 0x3002);
#endif
}

inline void read_binary_transaction(std::istream& in, transaction_t * xact,
				    char *& string_pool)
{
  xact->account = accounts[read_binary_number<account_t::ident_t>(in) - 1];
  xact->account->add_transaction(xact);

  read_binary_amount(in, xact->amount);

  if (read_binary_number<char>(in) == 1) {
    xact->cost = new amount_t;
    read_binary_amount(in, *xact->cost);
  } else {
    xact->cost = NULL;
  }
  read_binary_number(in, xact->flags);
  xact->flags |= TRANSACTION_BULK_ALLOC;
  init_binary_string(string_pool, &xact->note);

  xact->data = NULL;
}

inline void read_binary_entry(std::istream& in, entry_t * entry,
			      transaction_t *& xact_pool, char *& string_pool)
{
  read_binary_number(in, entry->date);
  read_binary_number(in, entry->state);
  init_binary_string(string_pool, &entry->code);
  init_binary_string(string_pool, &entry->payee);

  new(&entry->transactions) transactions_list;

  for (unsigned long i = 0, count = read_binary_number<unsigned long>(in);
       i < count;
       i++) {
    read_binary_transaction(in, xact_pool, string_pool);
    entry->add_transaction(xact_pool++);
  }
}

inline commodity_t * read_binary_commodity(std::istream& in)
{
  commodity_t * commodity = new commodity_t;
  *commodities_next++ = commodity;

  commodity->ident = read_binary_number<commodity_t::ident_t>(in);

  read_binary_string(in, commodity->symbol);
  read_binary_string(in, commodity->name);
  read_binary_string(in, commodity->note);
  read_binary_number(in, commodity->precision);
  read_binary_number(in, commodity->flags);

  for (unsigned long i = 0, count = read_binary_number<unsigned long>(in);
       i < count;
       i++) {
    std::time_t when;
    read_binary_number(in, when);
    amount_t amt;
    read_binary_amount(in, amt);
    commodity->history.insert(history_pair(when, amt));
  }

  read_binary_number(in, commodity->last_lookup);
  read_binary_amount(in, commodity->conversion);

  return commodity;
}

inline
account_t * read_binary_account(std::istream& in, account_t * master = NULL)
{
  account_t * acct = new account_t(NULL);
  *accounts_next++ = acct;

  acct->ident = read_binary_number<account_t::ident_t>(in);

  account_t::ident_t id;
  read_binary_number(in, id);	// parent id
  if (id == 0xffffffff)
    acct->parent = NULL;
  else
    acct->parent = accounts[id - 1];

  read_binary_string(in, acct->name);
  read_binary_string(in, acct->note);
  read_binary_number(in, acct->depth);

  // If all of the subaccounts will be added to a different master
  // account, throw away what we've learned about the recorded
  // journal's own master account.

  if (master) {
    delete acct;
    acct = master;
  }

  for (account_t::ident_t i = 0,
	 count = read_binary_number<account_t::ident_t>(in);
       i < count;
       i++) {
    account_t * child = read_binary_account(in);
    child->parent = acct;
    acct->add_account(child);
  }

  return acct;
}

unsigned int read_binary_journal(std::istream&	    in,
				 const std::string& file,
				 journal_t *	    journal,
				 account_t *	    master)
{
  account_index   =
  commodity_index = 0;

  // Read in the files that participated in this journal, so that they
  // can be checked for changes on reading.

  if (! file.empty()) {
    for (unsigned short i = 0,
	   count = read_binary_number<unsigned short>(in);
	 i < count;
	 i++) {
      std::string path = read_binary_string(in);
      if (i == 0 && path != file)
	return 0;

      std::time_t old_mtime;
      read_binary_number(in, old_mtime);
      struct stat info;
      stat(path.c_str(), &info);
      if (std::difftime(info.st_mtime, old_mtime) > 0)
	return 0;

      journal->sources.push_back(path);
    }
  }

  // Read in the accounts

  account_t::ident_t a_count = read_binary_number<account_t::ident_t>(in);
  accounts = accounts_next = new (account_t *)[a_count];
  journal->master = read_binary_account(in, master);

  // Read in the string pool

  unsigned long string_size = read_binary_number<unsigned long>(in);

  char * string_pool = new char[string_size];
  char * string_next = string_pool;

  in.read(string_pool, string_size);

  // Allocate the memory needed for the entries and transactions in
  // one large block, which is then chopped up and custom constructed
  // as necessary.

  unsigned long count        = read_binary_number<unsigned long>(in);
  unsigned long xact_count   = read_binary_number<unsigned long>(in);
  unsigned long bigint_count = read_binary_number<unsigned long>(in);

  std::size_t pool_size = (sizeof(entry_t) * count +
			   sizeof(transaction_t) * xact_count +
			   sizeof_bigint_t() * bigint_count);

  char * item_pool = new char[pool_size];

  entry_t *	  entry_pool = (entry_t *) item_pool;
  transaction_t * xact_pool  = (transaction_t *) (item_pool +
						  sizeof(entry_t) * count);
  bigints_index = 0;
  bigints = bigints_next
    = (amount_t::bigint_t *) (item_pool + sizeof(entry_t) * count +
			      sizeof(transaction_t) * xact_count);

  // Read in the commodities

  commodity_t::ident_t c_count = read_binary_number<commodity_t::ident_t>(in);
  commodities = commodities_next = new (commodity_t *)[c_count];
  for (commodity_t::ident_t i = 0; i < c_count; i++) {
    commodity_t * commodity = read_binary_commodity(in);
    std::pair<commodities_map::iterator, bool> result
      = commodity_t::commodities.insert(commodities_pair(commodity->symbol,
							 commodity));
    assert(result.second);
  }

  // Read in the entries and transactions

  for (unsigned long i = 0; i < count; i++) {
    read_binary_entry(in, entry_pool, xact_pool, string_next);
    journal->entries.push_back(entry_pool++);
  }

  assert(string_next == string_pool + string_size);

  // Clean up and return the number of entries read

  journal->item_pool	 = item_pool;
  journal->item_pool_end = item_pool + pool_size;

  delete[] accounts;
  delete[] commodities;
  delete[] string_pool;

  return count;
}

bool binary_parser_t::test(std::istream& in) const
{
  if (read_binary_number<unsigned long>(in) == binary_magic_number &&
      read_binary_number<unsigned long>(in) == format_version)
    return true;

  in.seekg(0);
  return false;
}

unsigned int binary_parser_t::parse(std::istream&	in,
				    journal_t *		journal,
				    account_t *		master,
				    const std::string * original_file)
{
  return read_binary_journal(in, original_file ? *original_file : "",
			     journal, master);
}

#if DEBUG_LEVEL >= ALPHA
#define write_binary_guard(in, id) {		\
  unsigned short guard = id;			\
  out.write((char *)&guard, sizeof(guard));	\
}
#else
#define write_binary_guard(in, id)
#endif

template <typename T>
inline void write_binary_number(std::ostream& out, T num) {
  out.write((char *)&num, sizeof(num));
}

inline void write_binary_string(std::ostream& out, const std::string& str)
{
  write_binary_guard(out, 0x3001);

  unsigned long len = str.length();
  if (len > 255) {
    assert(len < 65536);
    write_binary_number<unsigned char>(out, 0xff);
    write_binary_number<unsigned short>(out, len);
  } else {
    write_binary_number<unsigned char>(out, len);
  }

  if (len)
    out.write(str.c_str(), len);

  write_binary_guard(out, 0x3002);
}

void write_binary_amount(std::ostream& out, const amount_t& amt)
{
  if (amt.commodity)
    write_binary_number(out, amt.commodity->ident);
  else
    write_binary_number<commodity_t::ident_t>(out, 0xffffffff);

  amt.write_quantity(out);
}

void write_binary_transaction(std::ostream& out, transaction_t * xact)
{
  write_binary_number(out, xact->account->ident);
  write_binary_amount(out, xact->amount);
  if (xact->cost) {
    write_binary_number<char>(out, 1);
    write_binary_amount(out, *xact->cost);
  } else {
    write_binary_number<char>(out, 0);
  }
  write_binary_number(out, xact->flags);
}

void write_binary_entry(std::ostream& out, entry_t * entry)
{
  write_binary_number(out, entry->date);
  write_binary_number(out, entry->state);

  write_binary_number<unsigned long>(out, entry->transactions.size());
  for (transactions_list::const_iterator i = entry->transactions.begin();
       i != entry->transactions.end();
       i++)
    write_binary_transaction(out, *i);
}

void write_binary_commodity(std::ostream& out, commodity_t * commodity)
{
  commodity->ident = ++commodity_index;

  write_binary_number(out, commodity->ident);
  write_binary_string(out, commodity->symbol);
  write_binary_string(out, commodity->name);
  write_binary_string(out, commodity->note);
  write_binary_number(out, commodity->precision);
  write_binary_number(out, commodity->flags);

  write_binary_number<unsigned long>(out, commodity->history.size());
  for (history_map::const_iterator i = commodity->history.begin();
       i != commodity->history.end();
       i++) {
    write_binary_number(out, (*i).first);
    write_binary_amount(out, (*i).second);
  }

  write_binary_number(out, commodity->last_lookup);
  write_binary_amount(out, commodity->conversion);
}

static inline account_t::ident_t count_accounts(account_t * account)
{
  account_t::ident_t count = 1;

  for (accounts_map::iterator i = account->accounts.begin();
       i != account->accounts.end();
       i++)
    count += count_accounts((*i).second);

  return count;
}

void write_binary_account(std::ostream& out, account_t * account)
{
  account->ident = ++account_index;

  write_binary_number(out, account->ident);
  if (account->parent)
    write_binary_number(out, account->parent->ident);
  else
    write_binary_number<account_t::ident_t>(out, 0xffffffff);

  write_binary_string(out, account->name);
  write_binary_string(out, account->note);
  write_binary_number(out, account->depth);

  write_binary_number<account_t::ident_t>(out, account->accounts.size());
  for (accounts_map::iterator i = account->accounts.begin();
       i != account->accounts.end();
       i++)
    write_binary_account(out, (*i).second);
}

void write_binary_journal(std::ostream& out, journal_t * journal,
			  strings_list * files)
{
  write_binary_number(out, binary_magic_number);
  write_binary_number(out, format_version);

  // Write out the files that participated in this journal, so that
  // they can be checked for changes on reading.

  if (! files) {
    write_binary_number<unsigned short>(out, 0);
  } else {
    write_binary_number<unsigned short>(out, files->size());
    for (strings_list::const_iterator i = files->begin();
	 i != files->end();
	 i++) {
      write_binary_string(out, *i);
      struct stat info;
      stat((*i).c_str(), &info);
      write_binary_number(out, std::time_t(info.st_mtime));
    }
  }

  // Write out the accounts

  write_binary_number<account_t::ident_t>(out, count_accounts(journal->master));
  write_binary_account(out, journal->master);

  // Write out the string pool

  unsigned long xact_count = 0;

  std::ostream::pos_type string_pool_val = out.tellp();
  write_binary_number<unsigned long>(out, 0);

  for (entries_list::const_iterator i = journal->entries.begin();
       i != journal->entries.end();
       i++) {
    write_binary_string(out, (*i)->code);
    write_binary_string(out, (*i)->payee);

    for (transactions_list::const_iterator j = (*i)->transactions.begin();
	 j != (*i)->transactions.end();
	 j++) {
      xact_count++;
      write_binary_string(out, (*j)->note);
    }
  }

  unsigned long string_pool_size = (((unsigned long) out.tellp()) -
				    ((unsigned long) string_pool_val) -
				    sizeof(unsigned long));

  // Write out the number of entries, transactions, and amounts

  write_binary_number<unsigned long>(out, journal->entries.size());
  write_binary_number<unsigned long>(out, xact_count);
  std::ostream::pos_type bigints_val = out.tellp();
  write_binary_number<unsigned long>(out, 0);
  bigints_count = 0;

  // Write out the commodities

  write_binary_number<commodity_t::ident_t>
    (out, commodity_t::commodities.size() - 1);

  for (commodities_map::const_iterator i = commodity_t::commodities.begin();
       i != commodity_t::commodities.end();
       i++)
    if (! (*i).first.empty())
      write_binary_commodity(out, (*i).second);

  // Write out the entries and transactions

  for (entries_list::const_iterator i = journal->entries.begin();
       i != journal->entries.end();
       i++)
    write_binary_entry(out, *i);

  // Back-patch the count for amounts

  out.seekp(string_pool_val);
  write_binary_number<unsigned long>(out, string_pool_size);

  out.seekp(bigints_val);
  write_binary_number<unsigned long>(out, bigints_count);
}

} // namespace ledger
