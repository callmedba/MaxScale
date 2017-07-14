/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#define MXS_MODULE_NAME "masking"
#include "maskingrules.hh"
#include <algorithm>
#include <errno.h>
#include <functional>
#include <string.h>
#include <maxscale/debug.h>
#include <maxscale/jansson.hh>
#include <maxscale/mysql_utils.h>
#include <maxscale/pcre2.hh>
#include <maxscale/utils.hh>
#include <maxscale/json_api.h>

using std::auto_ptr;
using std::string;
using std::vector;
using std::tr1::shared_ptr;
using maxscale::Closer;

namespace
{

static const char MASKING_DEFAULT_FILL[]    = "X";

static const char KEY_APPLIES_TO[] = "applies_to";
static const char KEY_COLUMN[]     = "column";
static const char KEY_DATABASE[]   = "database";
static const char KEY_EXEMPTED[]   = "exempted";
static const char KEY_FILL[]       = "fill";
static const char KEY_REPLACE[]    = "replace";
static const char KEY_RULES[]      = "rules";
static const char KEY_TABLE[]      = "table";
static const char KEY_VALUE[]      = "value";
static const char KEY_WITH[]       = "with";
static const char KEY_OBFUSCATE[]  = "obfuscate";
static const char KEY_CAPTURE[]    = "capture";

/**
 * @class AccountVerbatim
 *
 * Implementation of @c MaskingRules::Rule::Account that compares user and
 * host names verbatim, that is, without regexp matching.
 */
class AccountVerbatim : public MaskingRules::Rule::Account
{
public:
    ~AccountVerbatim()
    {
    }

    static shared_ptr<MaskingRules::Rule::Account> create(const string& user, const string& host)
    {
        return shared_ptr<MaskingRules::Rule::Account>(new AccountVerbatim(user, host));
    }

    string user() const
    {
        return m_user;
    }

    string host() const
    {
        return m_host;
    }

    bool matches(const char* zUser, const char* zHost) const
    {
        ss_dassert(zUser);
        ss_dassert(zHost);

        return
            (m_user.empty() || (m_user == zUser)) &&
            (m_host.empty() || (m_host == zHost));
    }

private:
    AccountVerbatim(const string& user, const string& host)
        : m_user(user)
        , m_host(host)
    {
    }

    AccountVerbatim(const AccountVerbatim&);
    AccountVerbatim& operator = (const AccountVerbatim&);

private:
    string m_user;
    string m_host;
};


/**
 * @class AccountRegexp
 *
 * Implementation of @c MaskingRules::Rule::Account that compares user names
 * verbatim, that is, without regexp matching, and host names using regexp
 * matching.
 */
class AccountRegexp : public MaskingRules::Rule::Account
{
public:
    ~AccountRegexp()
    {
        pcre2_code_free(m_pCode);
    }

    static shared_ptr<MaskingRules::Rule::Account> create(const string& user, const string& host)
    {
        shared_ptr<MaskingRules::Rule::Account> sAccount;

        int errcode;
        PCRE2_SIZE erroffset;
        pcre2_code* pCode = pcre2_compile((PCRE2_SPTR)host.c_str(), PCRE2_ZERO_TERMINATED, 0,
                                          &errcode, &erroffset, NULL);

        if (pCode)
        {
            Closer<pcre2_code*> code(pCode);

            sAccount = shared_ptr<AccountRegexp>(new AccountRegexp(user, host, pCode));

            // Ownership of pCode has been moved to the AccountRegexp object.
            code.release();
        }
        else
        {
            PCRE2_UCHAR errbuf[512];
            pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
            MXS_ERROR("Regex compilation failed at %d for regex '%s': %s",
                      (int)erroffset, host.c_str(), errbuf);
        }

        return sAccount;
    }

    string user() const
    {
        return m_user;
    }

    string host() const
    {
        return m_host;
    }

    bool matches(const char* zUser, const char* zHost) const
    {
        ss_dassert(zUser);
        ss_dassert(zHost);

        bool rv = (m_user.empty() || (m_user == zUser));

        if (rv)
        {
            ss_dassert(m_pCode);
            pcre2_match_data* pData = pcre2_match_data_create_from_pattern(m_pCode, NULL);

            if (pData)
            {
                Closer<pcre2_match_data*> data(pData);

                rv = (pcre2_match(m_pCode, (PCRE2_SPTR)zHost, 0, 0, 0, pData, NULL) >= 0);
            }
        }

        return rv;
    }

private:
    AccountRegexp(const string& user,
                  const string& host,
                  pcre2_code* pCode)
        : m_user(user)
        , m_host(host)
        , m_pCode(pCode)
    {
    }

    AccountRegexp(const AccountRegexp&);
    AccountRegexp& operator = (const AccountRegexp&);

private:
    string      m_user;
    string      m_host;
    pcre2_code* m_pCode;
};

/**
 * Create MaxskingRules::Rule::Account instance
 *
 * @param zAccount  The account name as specified in the JSON rules file.
 *
 * @return Either an AccountVerbatim or AccountRegexp, depending on whether
 *         the provided account name contains wildcards or not.
 */
shared_ptr<MaskingRules::Rule::Account> create_account(const char* zAccount)
{
    shared_ptr<MaskingRules::Rule::Account> sAccount;

    size_t len = strlen(zAccount);
    char account[len + 1];
    strcpy(account, zAccount);

    char* zAt = strchr(account, '@');
    char* zUser = account;
    char* zHost = NULL;

    if (zAt)
    {
        *zAt = 0;
        zHost = zAt + 1;
    }

    if (mxs_mysql_trim_quotes(zUser))
    {
        char pcre_host[2 * len + 1]; // Surely enough

        mxs_mysql_name_kind_t rv = MXS_MYSQL_NAME_WITHOUT_WILDCARD;

        if (zHost)
        {
            if (mxs_mysql_trim_quotes(zHost))
            {
                rv = mxs_mysql_name_to_pcre(pcre_host, zHost, MXS_PCRE_QUOTE_WILDCARD);

                if (rv == MXS_MYSQL_NAME_WITH_WILDCARD)
                {
                    zHost = pcre_host;
                }
            }
            else
            {
                MXS_ERROR("Could not trim quotes from host part of %s.", zAccount);
                zHost = NULL;
            }
        }
        else
        {
            zHost = const_cast<char*>("");
        }

        if (zHost)
        {
            if (rv == MXS_MYSQL_NAME_WITH_WILDCARD)
            {
                sAccount = AccountRegexp::create(zUser, zHost);
            }
            else
            {
                sAccount = AccountVerbatim::create(zUser, zHost);
            }
        }
    }
    else
    {
        MXS_ERROR("Could not trim quotes from user part of %s.", zAccount);
    }

    return sAccount;
}

/**
 * Converts a list of account names into a vector of Account instances.
 *
 * @param zName     The key of the JSON array we are processing (error reporting).
 * @param pString   A JSON array of account names.
 * @param accounts  Vector of Account instances, to be filled by this function.
 *
 * @return True, if all account names could be converted, false otherwise.
 */
bool get_accounts(const char* zName,
                  json_t* pStrings,
                  vector<shared_ptr<MaskingRules::Rule::Account> >& accounts)
{
    ss_dassert(json_is_array(pStrings));

    bool success = true;

    size_t n = json_array_size(pStrings);
    size_t i = 0;

    while (success && (i < n))
    {
        json_t* pString = json_array_get(pStrings, i);
        ss_dassert(pString);

        if (json_is_string(pString))
        {
            shared_ptr<MaskingRules::Rule::Account> sAccount = create_account(json_string_value(pString));

            if (sAccount)
            {
                accounts.push_back(sAccount);
            }
            else
            {
                success = false;
            }
        }
        else
        {
            MXS_ERROR("An element in a '%s' array is not a string.", zName);
            success = false;
        }

        ++i;
    }

    return success;
}

/**
 * Create all MaskingRules::Rule instances
 *
 * @param pRules  A JSON array representing 'rules' from the rules file.
 * @param rules   Vector where corresponding Rule instances will be pushed.
 *
 * @return True, if all rules could be created.
 */
bool create_rules_from_array(json_t* pRules, vector<shared_ptr<MaskingRules::Rule> >& rules)
{
    ss_dassert(json_is_array(pRules));

    bool parsed = true;

    size_t n = json_array_size(pRules);
    size_t i = 0;

    while (parsed && (i < n))
    {
        json_t* pRule = json_array_get(pRules, i);
        ss_dassert(pRule);

        if (json_is_object(pRule))
        {
            auto_ptr<MaskingRules::Rule> sRule;
            json_t* pObfuscate = json_object_get(pRule, KEY_OBFUSCATE);
            json_t* pReplace = json_object_get(pRule, KEY_REPLACE);

            // Check whether we have KEY_OBFUSCATE or KEY_REPLACE
            if (!pReplace && !pObfuscate)
            {
                MXS_ERROR("A masking rule does not contain a '%s' or '%s' key.",
                          KEY_OBFUSCATE,
                          KEY_REPLACE);
                parsed = false;
                continue;
            }

            // Obfuscate takes the precedence
            if (pObfuscate)
            {
                sRule = MaskingRules::ObfuscateRule::create_from(pRule);
            }
            else
            {
                json_t* pCapture = json_object_get(pReplace, KEY_CAPTURE);
                // Capture takes the precedence
                sRule = pCapture ?
                        MaskingRules::CaptureRule::create_from(pRule) :
                        MaskingRules::ReplaceRule::create_from(pRule);
            }

            if (sRule.get())
            {
                rules.push_back(shared_ptr<MaskingRules::Rule>(sRule.release()));
            }
            else
            {
                parsed = false;
            }
        }
        else
        {
            MXS_ERROR("Element %lu of the '%s' array is not an object.", i, KEY_RULES);
            parsed = false;
        }

        ++i;
    }

    return parsed;
}

/**
 * Create all MaskingRules::Rule instances
 *
 * @param pRoo   A JSON object, representing the rules file.
 * @param rules  Vector where all Rule instances will be pushed.
 *
 * @return True, if all rules could be created.
 */
bool create_rules_from_root(json_t* pRoot,
                            vector<shared_ptr<MaskingRules::Rule> >& rules)
{
    bool parsed = false;
    json_t* pRules = json_object_get(pRoot, KEY_RULES);

    if (pRules)
    {
        if (json_is_array(pRules))
        {
            parsed = create_rules_from_array(pRules, rules);
        }
        else
        {
            MXS_ERROR("The masking rules object contains a `%s` key, "
                      "but it is not an array.",
                      KEY_RULES);
        }
    }

    return parsed;
}

}

//
// MaskingRules::Rule::Account
//

MaskingRules::Rule::Account::Account()
{
}

MaskingRules::Rule::Account::~Account()
{
}

//
// MaskingRules::Rule
//

MaskingRules::Rule::Rule(const std::string& column,
                         const std::string& table,
                         const std::string& database,
                         const std::vector<SAccount>& applies_to,
                         const std::vector<SAccount>& exempted)
    : m_column(column)
    , m_table(table)
    , m_database(database)
    , m_applies_to(applies_to)
    , m_exempted(exempted)
{
}

MaskingRules::ReplaceRule::ReplaceRule(const std::string& column,
                                       const std::string& table,
                                       const std::string& database,
                                       const std::vector<SAccount>& applies_to,
                                       const std::vector<SAccount>& exempted,
                                       const std::string& value,
                                       const std::string& fill)
    : MaskingRules::Rule::Rule(column, table, database, applies_to, exempted)
    , m_value(value)
    , m_fill(fill)
{
}

MaskingRules::ObfuscateRule::ObfuscateRule(const std::string& column,
                                           const std::string& table,
                                           const std::string& database,
                                           const std::vector<SAccount>& applies_to,
                                           const std::vector<SAccount>& exempted)
    : MaskingRules::Rule::Rule(column, table, database, applies_to, exempted)
{
}

MaskingRules::CaptureRule::CaptureRule(const std::string& column,
                                       const std::string& table,
                                       const std::string& database,
                                       const std::vector<SAccount>& applies_to,
                                       const std::vector<SAccount>& exempted,
                                       pcre2_code* regexp,
                                       const std::string& fill)
    : MaskingRules::Rule::Rule(column, table, database, applies_to, exempted)
    , m_regexp(regexp)
    , m_fill(fill)
{
}

MaskingRules::Rule::~Rule()
{
}

MaskingRules::ReplaceRule::~ReplaceRule()
{
}

MaskingRules::ObfuscateRule::~ObfuscateRule()
{
}

MaskingRules::CaptureRule::~CaptureRule()
{
    pcre2_code_free(m_regexp);
}

/** Check the Json array for user rules
 *
 * @param pApplies_to    The array of users the rule is applied to
 * @param pExempted      The array of users the rule is NOT applied to
 *
 * @return               False on errors, True otherwise
 */
static bool validate_user_rules(json_t* pApplies_to, json_t* pExempted)
{
    const char *err = NULL;
    // Check for pApplies_to and pExempted
    if (pApplies_to && !json_is_array(pApplies_to))
    {
        err = KEY_APPLIES_TO;
    }

    if (pExempted && !json_is_array(pExempted))
    {
        err = KEY_EXEMPTED;
    }

    if (err)
    {
        MXS_ERROR("A masking rule contains a '%s' key, "
                  "but the value is not an array.",
                  err);
        return false;
    }

    return true;
}

static json_t* rule_get_object(json_t* pRule,
                               const char *rule_type)
{
    json_t *pObj = NULL;
    // Check 'rule_type' object
    if (!pRule || !(pObj = json_object_get(pRule, rule_type)))
    {
        MXS_ERROR("A masking rule does not contain the '%s' key.",
                  rule_type);
        return NULL;
    }
    if (!json_is_object(pObj))
    {
        MXS_ERROR("A masking rule contains a '%s' key, "
                  "but the value is not a valid Json object.",
                  rule_type);
        return NULL;
    }
    return pObj;
}

/**
 * Checks database, table and column values
 *
 * @param pColumn    The database column
 * @param pTable     The database table
 * @param pDatabase  The database name
 *
 * @return           true on success, false otherwise
 */
static bool rule_check_database_options(json_t* pColumn,
                                        json_t* pTable,
                                        json_t* pDatabase)
{

    // Only column is mandatory; both table and database are optional.
    if ((pColumn && json_is_string(pColumn)) &&
        (!pTable || json_is_string(pTable)) &&
        (!pDatabase || json_is_string(pDatabase)))
    {
        return true;
    }
    else
    {
        if (!pColumn || !json_is_string(pColumn))
        {
            MXS_ERROR("The '%s' object of a masking rule does not have "
                      "the mandatory '%s' key or it's not a valid Json string.",
                      KEY_REPLACE,
                      KEY_COLUMN);
        }
        else
        {
            MXS_ERROR("In the '%s' object of a masking rule, the keys "
                      "'%s' and/or '%s' re not valid Json strings.",
                      KEY_REPLACE,
                      KEY_TABLE,
                      KEY_DATABASE);
       }
       return false;
    }
}

/**
 * Returns a Json objet with the fill value
 *
 * @param pDoc    The Json input object
 *
 * @return        A Json object or NULL
 */
static json_t* rule_get_fill(json_t* pDoc)
{
    json_t* pFill = json_object_get(pDoc, KEY_FILL);

    if (!pFill)
    {
        // Allowed. Use default value for fill and add it to pWith.
        pFill = json_string(MASKING_DEFAULT_FILL);
        if (pFill)
        {
            json_object_set_new(pDoc, KEY_FILL, pFill);
        }
        else
        {
            MXS_ERROR("json_string() error, cannot produce"
                      " a valid '%s' object for rule '%s'.",
                      KEY_FILL,
                      KEY_REPLACE);
        }
    }

    return pFill;
}

/**
 * Perform rule checks for all Rule classes
 *
 * @param pRule         The Json rule
 * @param applies_to    Account instances corresponding to the
 *                      accounts listed in 'applies_to' in the json file.
 * @param exempted      Account instances corresponding to the
 *                      accounts listed in 'exempted' in the json file.
 *
 * @return              True on success, false on errors.
 */
static bool rule_run_common_checks(json_t* pRule,
                                   vector<shared_ptr<MaskingRules::Rule::Account> >* applies_to,
                                   vector<shared_ptr<MaskingRules::Rule::Account> >* exempted)
{
    json_t* pApplies_to = json_object_get(pRule, KEY_APPLIES_TO);
    json_t* pExempted = json_object_get(pRule, KEY_EXEMPTED);

    // Check for pApplies_to and pExempted
    if (!validate_user_rules(pApplies_to, pExempted))
    {
        return false;
    }

    // Set the account rules
    if (pApplies_to && pExempted &&
        (!get_accounts(KEY_APPLIES_TO, pApplies_to, *applies_to) ||
         !get_accounts(KEY_EXEMPTED, pExempted, *exempted)))
    {
        return false;
    }

    return true;
}

/**
 * Returns rule values from a Json rule object
 *
 * @param pRule         The Json rule
 * @param column        The column value from the json file.
 * @param table         The table value from the json file.
 * @param database      The database value from the json file.
 *
 * @return              True on success, false on errors.
 */
static bool rule_get_common_values(json_t* pRule,
                                   std::string* column,
                                   std::string* table,
                                   std::string* database)
{
    // Get database, table && column
    json_t* pDatabase = json_object_get(pRule, KEY_DATABASE);
    json_t* pTable = json_object_get(pRule, KEY_TABLE);
    json_t* pColumn = json_object_get(pRule, KEY_COLUMN);

    // Check column/table/dataase
    if (!rule_check_database_options(pColumn,
                                     pTable,
                                     pDatabase))
    {
        return false;
    }

    // Column exists
    column->assign(json_string_value(pColumn));

    // Check optional table and dbname
    if (pTable)
    {
        table->assign(json_string_value(pTable));
    }
    if (pDatabase)
    {
        database->assign(json_string_value(pDatabase));
    }

    return true;
}

/**
 * Check Json object, run common checks and return rule values
 *
 * @param pRule         The Json rule object
 * @param applies_to    Account instances corresponding to the
 *                      accounts listed in 'applies_to' in the json file.
 * @param exempted      Account instances corresponding to the
 *                      accounts listed in 'exempted' in the json file.
 * @param column        The column value from the json file.
 * @param table         The table value from the json file.
 * @param database      The database value from the json file.
 * @param rule_type     The rule_type (obfuscate or replace)
 *
 * @return              True on success, false on errors
 */
bool rule_get_values(json_t* pRule,
                     vector<shared_ptr<MaskingRules::Rule::Account> >* applies_to,
                     vector<shared_ptr<MaskingRules::Rule::Account> >* exempted,
                     std::string* column,
                     std::string* table,
                     std::string* database,
                     const char *rule_type)
{
    json_t *pKeyObj;
    // Get Key object based on 'rule_type' param
    if ((pKeyObj = rule_get_object(pRule,
                                   rule_type)) &&
        // Run checks on user access
        rule_run_common_checks(pRule,
                               applies_to,
                               exempted)       &&
        // Extract values from the rule
        rule_get_common_values(pKeyObj,
                               column,
                               table,
                               database))
    {
        return true;
    }

    return false;
}

/**
 * Returns 'capture' regexp & 'fill' value from a 'replace' rule
 *
 * @param pRule       The Json rule doc
 * @param pCapture    The string buffer for 'capture'value
 * @param pFill       The string buffer for 'fill' value
 *
 * @return            True on success, false on errors
 */
bool rule_get_capture_fill(json_t* pRule,
                           std::string *pCapture,
                           std::string* pFill)
{
    // Get the 'with' key from the rule
    json_t* pWith = json_object_get(pRule, KEY_WITH);
    if (!pWith || !json_is_object(pWith))
    {
        MXS_ERROR("A masking '%s' rule doesn't have a valid '%s' key",
                  KEY_REPLACE,
                  KEY_WITH);
        return false;
    }

    // Get the 'replace' rule object
    json_t* pKeyObj;
    if (!(pKeyObj = rule_get_object(pRule, KEY_REPLACE)))
    {
        return false;
    }

    // Get fill from 'with' object
    json_t* pTheFill = rule_get_fill(pWith);
    // Get 'capture' from 'replace' ojbect
    json_t* pTheCapture = json_object_get(pKeyObj, KEY_CAPTURE);

    // Check values
    if ((!pTheFill || !json_is_string(pTheFill)) ||
        ((!pTheCapture || !json_is_string(pTheCapture))))
    {
        MXS_ERROR("A masking '%s' rule has '%s' and/or '%s' "
                  "invalid Json strings.",
                  KEY_REPLACE,
                  KEY_CAPTURE,
                  KEY_FILL);
        return false;
    }
    else
    {
        // Update the string buffers
        pFill->assign(json_string_value(pTheFill));
        pCapture->assign(json_string_value(pTheCapture));

        return true;
    }
}

/**
 * Returns 'value' & 'fill' from a 'replace' rule
 *
 * @param pRule     The Json rule doc
 * @param pValue    The string buffer for 'value'
 * @param pFill     The string buffer for 'fill'
 *
 * @return            True on success, false on errors
 */
bool rule_get_value_fill(json_t* pRule,
                         std::string *pValue,
                         std::string* pFill)
{
    // Get the 'with' key from the rule
    json_t* pWith = json_object_get(pRule, KEY_WITH);
    if (!pWith || !json_is_object(pWith))
    {
        MXS_ERROR("A masking '%s' rule doesn't have a valid '%s' key.",
                  KEY_REPLACE,
                  KEY_WITH);
        return false;
    }

    // Get fill from 'with' object
    json_t* pTheFill = rule_get_fill(pWith);

    // Get value from 'with' object
    json_t* pTheValue = json_object_get(pWith, KEY_VALUE);

    // Check values
    if ((!pTheFill || !json_is_string(pTheFill)) ||
        (!pTheValue || !json_is_string(pTheValue)))
    {
        MXS_ERROR("A masking '%s' rule has '%s' and/or '%s' "
                  "invalid Json strings.",
                  KEY_REPLACE,
                  KEY_VALUE,
                  KEY_FILL);
        return false;
    }
    else
    {
        // Update the string buffers
        pFill->assign(json_string_value(pTheFill));
        pValue->assign(json_string_value(pTheValue));
        return true;
    }
}

//static
auto_ptr<MaskingRules::Rule> MaskingRules::ReplaceRule::create_from(json_t* pRule)
{
    ss_dassert(json_is_object(pRule));

    json_t *pReplace;
    std::string column, table, database, value, fill;
    vector<shared_ptr<MaskingRules::Rule::Account> > applies_to;
    vector<shared_ptr<MaskingRules::Rule::Account> > exempted;
    auto_ptr<MaskingRules::Rule> sRule;

    // Check rule, extract base values
    if (rule_get_values(pRule,
                        &applies_to,
                        &exempted,
                        &column,
                        &table,
                        &database,
                        KEY_REPLACE) &&
        rule_get_value_fill(pRule, &value, &fill)) // get value/fill
    {
        if (!value.empty() && !fill.empty())
        {
            // Apply value/fill: instantiate the ReplaceRule class
            sRule = auto_ptr<MaskingRules::ReplaceRule>(new MaskingRules::ReplaceRule(column,
                                                                                      table,
                                                                                      database,
                                                                                      applies_to,
                                                                                      exempted,
                                                                                      value,
                                                                                      fill));
        }
        else
        {
            MXS_ERROR("Key '%s' or '%s' of masking '%s' rule object '%s' "
                      "has a non-string value or empty value.",
                      KEY_VALUE,
                      KEY_FILL,
                      KEY_REPLACE,
                      KEY_WITH);
        }
    }

    return sRule;
}

//static
auto_ptr<MaskingRules::Rule> MaskingRules::ObfuscateRule::create_from(json_t* pRule)
{
    ss_dassert(json_is_object(pRule));

    std::string column, table, database;
    vector<shared_ptr<MaskingRules::Rule::Account> > applies_to;
    vector<shared_ptr<MaskingRules::Rule::Account> > exempted;
    auto_ptr<MaskingRules::Rule> sRule;

    // Check rule, extract base values
    if (rule_get_values(pRule,
                        &applies_to,
                        &exempted,
                        &column,
                        &table,
                        &database,
                        KEY_OBFUSCATE))
    {
        sRule = auto_ptr<MaskingRules::Rule>(new MaskingRules::ObfuscateRule(column,
                                                                             table,
                                                                             database,
                                                                             applies_to,
                                                                             exempted));
    }

    return sRule;
}

/**
 * Compiles a pcre2 pattern match
 *
 * @param match_string    The pattern match to compile
 *
 * @return                A valid pcre2_code code or NULL on errors.
 */
static pcre2_code* rule_compile_pcre2_match(const char* match_string)
{
    int errcode;
    PCRE2_SIZE erroffset;
    // Compile regexp
    pcre2_code* pCode = pcre2_compile((PCRE2_SPTR)match_string,
                                      PCRE2_ZERO_TERMINATED,
                                      0,
                                      &errcode,
                                      &erroffset,
                                      NULL);
    if (!pCode)
    {
        PCRE2_UCHAR errbuf[512];
        pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
        MXS_ERROR("Regex compilation failed at %d for regex '%s': %s",
                  (int)erroffset, match_string, errbuf);
        return NULL;
    }

    return pCode;
}

//static
auto_ptr<MaskingRules::Rule> MaskingRules::CaptureRule::create_from(json_t* pRule)
{
    ss_dassert(json_is_object(pRule));

    std::string column, table, database, value, fill, capture;
    vector<shared_ptr<MaskingRules::Rule::Account> > applies_to;
    vector<shared_ptr<MaskingRules::Rule::Account> > exempted;
    auto_ptr<MaskingRules::Rule> sRule;

    // Check rule, extract base values
    // Note: the capture rule has same rule_type of "replace"
    if (rule_get_values(pRule,
                        &applies_to,
                        &exempted,
                        &column,
                        &table,
                        &database,
                        KEY_REPLACE) &&
        rule_get_capture_fill(pRule,  // get capture/fill
                              &capture,
                              &fill))
    {

        if (!capture.empty() && !fill.empty())
        {
            // Compile the regexp capture
            pcre2_code* pCode = rule_compile_pcre2_match(capture.c_str());

            if (pCode)
            {
                Closer<pcre2_code*> code(pCode);
                // Instantiate the CaptureRule class
                sRule = auto_ptr<MaskingRules::CaptureRule>(new MaskingRules::CaptureRule(column,
                                                                                          table,
                                                                                          database,
                                                                                          applies_to,
                                                                                          exempted,
                                                                                          pCode,
                                                                                          fill));

                // Ownership of pCode has been moved to the CaptureRule object.
                code.release();
            }
        }
    }

    return sRule;
}

string MaskingRules::Rule::match() const
{
    string s;

    s += m_database.empty() ? "*" : m_database;
    s += ".";
    s += m_table.empty() ? "*" : m_table;
    s += ".";
    s += m_column;

    return s;
}

namespace
{

class AccountMatcher : std::unary_function<MaskingRules::Rule::SAccount, bool>
{
public:
    AccountMatcher(const char* zUser, const char* zHost)
        : m_zUser(zUser)
        , m_zHost(zHost)
    {}

    bool operator()(const MaskingRules::Rule::SAccount& sAccount)
    {
        return sAccount->matches(m_zUser, m_zHost);
    }

private:
    const char* m_zUser;
    const char* m_zHost;
};

}

bool MaskingRules::Rule::matches(const ComQueryResponse::ColumnDef& column_def,
                                 const char* zUser,
                                 const char* zHost) const
{
    bool match =
        (m_column == column_def.org_name()) &&
        (m_table.empty() || (m_table == column_def.org_table())) &&
        (m_database.empty() || (m_database == column_def.schema()));

    if (match)
    {
        // If the column matched, then we need to check whether the rule applies
        // to the user and host.

        AccountMatcher matcher(zUser, zHost);

        if (m_applies_to.size() != 0)
        {
            match = false;

            vector<SAccount>::const_iterator i = std::find_if(m_applies_to.begin(),
                                                              m_applies_to.end(),
                                                              matcher);

            match = (i != m_applies_to.end());
        }

        if (match && (m_exempted.size() != 0))
        {
            // If it is still a match, we need to check whether the user/host is
            // exempted.

            vector<SAccount>::const_iterator i = std::find_if(m_exempted.begin(),
                                                              m_exempted.end(),
                                                              matcher);

            match = (i == m_exempted.end());
        }
    }

    return match;
}

/**
 * Basic obfuscation routine
 *
 * @param c    The bye to obfuscate
 *
 * @return     The obfuscated byte
 */
static inline char maxscale_basic_obfuscation(const char c)
{
    if (c >= 'a' && c <= 'z')
    {
        return (c - 'a' + 13) % 26 + 'a';
    }
    else if (c >= 'A' && c <= 'Z')
    {
        return (c - 'A' + 13) % 26 + 'A';
    }
    else
    {
        char d = c + 32;
        d = d > 127 ? 127 : d;
        return d;
    }
    return c;
}

/**
 * Fills a buffer with a fill string
 *
 * @param f_first    The iterator pointing to first fill byt
 * @param f_last     The iterator pointing to last fill byte
 * @param o_first    The iterator pointing to first buffer byte
 * @param o_last     The iterator pointing to last buffer byte
 */
template<class FillIter, class OutIter>
inline void fill_buffer(FillIter f_first,
                        FillIter f_last,
                        OutIter  o_first,
                        OutIter  o_last)
{
    FillIter pFill = f_first;
    while (o_first != o_last)
    {
        *o_first++ = *pFill++;
        if (pFill == f_last)
        {
            pFill = f_first;
        }
    }
}

void MaskingRules::CaptureRule::rewrite(LEncString& s) const
{
    int rv = 0;
    uint32_t n_matches = 0;
    PCRE2_SIZE* ovector = NULL;
    // Create the match data object from m_regexp class member
    pcre2_match_data* pData = pcre2_match_data_create_from_pattern(m_regexp, NULL);
    // Set initial offset to the input beginning
    PCRE2_SIZE startoffset = 0;
    // Get input string size
    size_t total_len = s.length();

    if (pData)
    {
        // Get the fill size
        size_t fill_len = m_fill.length();
        Closer<pcre2_match_data*> data(pData);

        // Match all the compiled pattern
        while ((startoffset < total_len) &&
               (rv = pcre2_match(m_regexp,
                                 (PCRE2_SPTR)s.to_string().c_str(),
                                 PCRE2_ZERO_TERMINATED,
                                 startoffset,
                                 0,
                                 pData,
                                 NULL)) >= 0)
        {
            // Get offset array value pairs of substrings: $0=0,1 ; $1=2,3
            PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(pData);

            // Get Full Match substring size: $0 is [0] and [1]
            size_t substring_len = ovector[1] - ovector[0];
            // Go to Full Match substring offset: 0
            LEncString::iterator i = s.begin() + ovector[0];

            // Avoid infinite loop in pcre2_match for a zero-length match
            if (ovector[1] == ovector[0])
            {
                break;
            }

            // Copy the fill string into substring
            fill_buffer(m_fill.begin(), m_fill.end(), i, i + substring_len);

            // Set offset to the end of Full Match substring or break
            startoffset = ovector[1];
        }

        // Log errors, exclding NO_MATCH or PARTIAL
        if (rv < 0 && (rv != PCRE2_ERROR_NOMATCH || PCRE2_ERROR_PARTIAL))
        {
            MXS_PCRE2_PRINT_ERROR(rv);
        }
    }
    else
    {
        MXS_ERROR("Allocation of matching data for PCRE2 failed."
                  " This is most likely caused by a lack of memory");
    }
}

void MaskingRules::ObfuscateRule::rewrite(LEncString& s) const
{
    // Basic Obfuscation routine
    std::transform(s.begin(),
                   s.end(),
                   s.begin(),
                   maxscale_basic_obfuscation);
}

void MaskingRules::ReplaceRule::rewrite(LEncString& s) const
{
    bool rewritten = false;

    size_t total_len = s.length();

    if (!m_value.empty())
    {
        if (m_value.length() == total_len)
        {
            std::copy(m_value.begin(), m_value.end(), s.begin());
            rewritten = true;
        }
    }

    if (!rewritten)
    {
        if (!m_fill.empty())
        {
            LEncString::iterator i = s.begin();
            size_t len = m_fill.length();

            // Copy the fill string
            fill_buffer(m_fill.begin(), m_fill.end(), s.begin(), s.end());
        }
        else
        {
            MXS_ERROR("Length of returned value \"%s\" is %u, while length of "
                      "replacement value \"%s\" is %u, and no 'fill' value specified.",
                      s.to_string().c_str(), (unsigned)s.length(),
                      m_value.c_str(), (unsigned)m_value.length());
        }
    }
}

//
// MaskingRules
//

MaskingRules::MaskingRules(json_t* pRoot, const std::vector<SRule>& rules)
    : m_pRoot(pRoot)
    , m_rules(rules)
{
    json_incref(m_pRoot);
}

MaskingRules::~MaskingRules()
{
    json_decref(m_pRoot);
}

//static
auto_ptr<MaskingRules> MaskingRules::load(const char* zPath)
{
    auto_ptr<MaskingRules> sRules;

    FILE* pFile = fopen(zPath, "r");

    if (pFile)
    {
        Closer<FILE*> file(pFile);

        json_error_t error;
        json_t* pRoot = json_loadf(file.get(), JSON_DISABLE_EOF_CHECK, &error);

        if (pRoot)
        {
            Closer<json_t*> root(pRoot);

            sRules = create_from(root.get());
        }
        else
        {
            MXS_ERROR("Loading rules file failed: (%s:%d:%d): %s",
                      zPath, error.line, error.column, error.text);
        }
    }
    else
    {
        MXS_ERROR("Could not open rules file %s for reading: %s",
                  zPath, mxs_strerror(errno));
    }

    return sRules;
}

//static
auto_ptr<MaskingRules> MaskingRules::parse(const char* zJson)
{
    auto_ptr<MaskingRules> sRules;

    json_error_t error;
    json_t* pRoot = json_loads(zJson, JSON_DISABLE_EOF_CHECK, &error);

    if (pRoot)
    {
        Closer<json_t*> root(pRoot);

        sRules = create_from(root.get());
    }
    else
    {
        MXS_ERROR("Parsing rules failed: (%d:%d): %s",
                  error.line, error.column, error.text);
    }

    return sRules;
}

//static
std::auto_ptr<MaskingRules> MaskingRules::create_from(json_t* pRoot)
{
    auto_ptr<MaskingRules> sRules;

    vector<SRule> rules;

    if (create_rules_from_root(pRoot, rules))
    {
        sRules = auto_ptr<MaskingRules>(new MaskingRules(pRoot, rules));
    }

    return sRules;
}

namespace
{

class RuleMatcher : std::unary_function<MaskingRules::SRule, bool>
{
public:
    RuleMatcher(const ComQueryResponse::ColumnDef& column_def,
                const char* zUser,
                const char* zHost)
        : m_column_def(column_def)
        , m_zUser(zUser)
        , m_zHost(zHost)
    {
    }

    bool operator()(const MaskingRules::SRule& sRule)
    {
        return sRule->matches(m_column_def, m_zUser, m_zHost);
    }

private:
    const ComQueryResponse::ColumnDef& m_column_def;
    const char* m_zUser;
    const char* m_zHost;
};

}

const MaskingRules::Rule* MaskingRules::get_rule_for(const ComQueryResponse::ColumnDef& column_def,
                                                     const char* zUser,
                                                     const char* zHost) const
{
    const Rule* pRule = NULL;

    RuleMatcher matcher(column_def, zUser, zHost);
    vector<SRule>::const_iterator i = std::find_if(m_rules.begin(), m_rules.end(), matcher);

    if (i != m_rules.end())
    {
        const SRule& sRule = *i;

        pRule = sRule.get();
    }

    return pRule;
}
