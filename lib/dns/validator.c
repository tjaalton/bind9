/*
 * Copyright (C) 2000-2002  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM
 * DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
 * INTERNET SOFTWARE CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: validator.c,v 1.117 2004/02/03 00:59:05 marka Exp $ */

#include <config.h>

#include <isc/mem.h>
#include <isc/print.h>
#include <isc/string.h>
#include <isc/task.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/ds.h>
#include <dns/dnssec.h>
#include <dns/events.h>
#include <dns/keytable.h>
#include <dns/log.h>
#include <dns/message.h>
#include <dns/ncache.h>
#include <dns/nsec.h>
#include <dns/rdata.h>
#include <dns/rdatastruct.h>
#include <dns/rdataset.h>
#include <dns/rdatatype.h>
#include <dns/resolver.h>
#include <dns/result.h>
#include <dns/validator.h>
#include <dns/view.h>

#define VALIDATOR_MAGIC			ISC_MAGIC('V', 'a', 'l', '?')
#define VALID_VALIDATOR(v)		ISC_MAGIC_VALID(v, VALIDATOR_MAGIC)

#define VALATTR_SHUTDOWN		0x0001
#define VALATTR_FOUNDNONEXISTENCE	0x0002
#define VALATTR_TRIEDVERIFY		0x0004
#define VALATTR_NEGATIVE		0x0008
#define VALATTR_INSECURITY		0x0010

#define VALATTR_NEEDNOQNAME		0x0100
#define VALATTR_NEEDNOWILDCARD		0x0200
#define VALATTR_NEEDNODATA		0x0400

#define VALATTR_FOUNDNOQNAME		0x1000
#define VALATTR_FOUNDNOWILDCARD		0x2000
#define VALATTR_FOUNDNODATA		0x4000

#define NEEDNODATA(val) ((val->attributes & VALATTR_NEEDNODATA) != 0)
#define NEEDNOQNAME(val) ((val->attributes & VALATTR_NEEDNOQNAME) != 0)
#define NEEDNOWILDCARD(val) ((val->attributes & VALATTR_NEEDNOWILDCARD) != 0)

#define SHUTDOWN(v)		(((v)->attributes & VALATTR_SHUTDOWN) != 0)

static void
destroy(dns_validator_t *val);

static isc_result_t
get_dst_key(dns_validator_t *val, dns_rdata_rrsig_t *siginfo,
	    dns_rdataset_t *rdataset);

static isc_result_t
validate(dns_validator_t *val, isc_boolean_t resume);

static isc_result_t
validatezonekey(dns_validator_t *val, isc_boolean_t resume);

static isc_result_t
nsecvalidate(dns_validator_t *val, isc_boolean_t resume);

static isc_result_t
proveunsecure(dns_validator_t *val, isc_boolean_t resume);

static void
validator_logv(dns_validator_t *val, isc_logcategory_t *category,
	       isc_logmodule_t *module, int level, const char *fmt, va_list ap)
     ISC_FORMAT_PRINTF(5, 0);

static void
validator_log(dns_validator_t *val, int level, const char *fmt, ...)
     ISC_FORMAT_PRINTF(3, 4);

static void
validator_logcreate(dns_validator_t *val,
		    dns_name_t *name, dns_rdatatype_t type,
		    const char *caller, const char *operation);

static void
validator_done(dns_validator_t *val, isc_result_t result) {
	isc_task_t *task;

	if (val->event == NULL)
		return;

	/*
	 * Caller must be holding the lock.
	 */

	val->event->result = result;
	task = val->event->ev_sender;
	val->event->ev_sender = val;
	val->event->ev_type = DNS_EVENT_VALIDATORDONE;
	val->event->ev_action = val->action;
	val->event->ev_arg = val->arg;
	isc_task_sendanddetach(&task, (isc_event_t **)&val->event);
}

static inline isc_boolean_t
exit_check(dns_validator_t *val) {
	/*
	 * Caller must be holding the lock.
	 */
	if (!SHUTDOWN(val))
		return (ISC_FALSE);

	INSIST(val->event == NULL);

	if (val->fetch != NULL || val->subvalidator != NULL)
		return (ISC_FALSE);

	return (ISC_TRUE);
}

static void
auth_nonpending(dns_message_t *message) {
	isc_result_t result;
	dns_name_t *name;
	dns_rdataset_t *rdataset;

	for (result = dns_message_firstname(message, DNS_SECTION_AUTHORITY);
	     result == ISC_R_SUCCESS;
	     result = dns_message_nextname(message, DNS_SECTION_AUTHORITY))
	{
		name = NULL;
		dns_message_currentname(message, DNS_SECTION_AUTHORITY, &name);
		for (rdataset = ISC_LIST_HEAD(name->list);
		     rdataset != NULL;
		     rdataset = ISC_LIST_NEXT(rdataset, link))
		{
			if (rdataset->trust == dns_trust_pending)
				rdataset->trust = dns_trust_authauthority;
		}
	}
}

static isc_boolean_t
isdelegation(dns_name_t *name, dns_rdataset_t *rdataset,
	     isc_result_t dbresult)
{
	dns_rdataset_t set;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	isc_boolean_t found;
	isc_result_t result;

	REQUIRE(dbresult == DNS_R_NXRRSET || dbresult == DNS_R_NCACHENXRRSET);

	dns_rdataset_init(&set);
	if (dbresult == DNS_R_NXRRSET)
		dns_rdataset_clone(rdataset, &set);
	else {
		result = dns_ncache_getrdataset(rdataset, name,
						dns_rdatatype_nsec, &set);
		if (result != ISC_R_SUCCESS)
			return (ISC_FALSE);
	}

	INSIST(set.type == dns_rdatatype_nsec);

	found = ISC_FALSE;
	result = dns_rdataset_first(&set);
	if (result == ISC_R_SUCCESS) {
		dns_rdataset_current(&set, &rdata);
		found = dns_nsec_typepresent(&rdata, dns_rdatatype_ns);
	}
	dns_rdataset_disassociate(&set);
	return (found);
}

static void
fetch_callback_validator(isc_task_t *task, isc_event_t *event) {
	dns_fetchevent_t *devent;
	dns_validator_t *val;
	dns_rdataset_t *rdataset;
	isc_boolean_t want_destroy;
	isc_result_t result;
	isc_result_t eresult;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_FETCHDONE);
	devent = (dns_fetchevent_t *)event;
	val = devent->ev_arg;
	rdataset = &val->frdataset;
	eresult = devent->result;

	isc_event_free(&event);
	dns_resolver_destroyfetch(&val->fetch);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in fetch_callback_validator");
	LOCK(&val->lock);
	if (eresult == ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "keyset with trust %d", rdataset->trust);
		/*
		 * Only extract the dst key if the keyset is secure.
		 */
		if (rdataset->trust >= dns_trust_secure) {
			result = get_dst_key(val, val->siginfo, rdataset);
			if (result == ISC_R_SUCCESS)
				val->keyset = &val->frdataset;
		}
		result = validate(val, ISC_TRUE);
		if (result != DNS_R_WAIT)
			validator_done(val, result);
	} else {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "fetch_callback_validator: got %s",
			      isc_result_totext(eresult));
		if (eresult == ISC_R_CANCELED)
			validator_done(val, eresult);
		else
			validator_done(val, DNS_R_NOVALIDKEY);
	}
	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy)
		destroy(val);
}

static void
dsfetched(isc_task_t *task, isc_event_t *event) {
	dns_fetchevent_t *devent;
	dns_validator_t *val;
	dns_rdataset_t *rdataset;
	isc_boolean_t want_destroy;
	isc_result_t result;
	isc_result_t eresult;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_FETCHDONE);
	devent = (dns_fetchevent_t *)event;
	val = devent->ev_arg;
	rdataset = &val->frdataset;
	eresult = devent->result;

	isc_event_free(&event);
	dns_resolver_destroyfetch(&val->fetch);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in dsfetched");
	LOCK(&val->lock);
	if (eresult == ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "dsset with trust %d", rdataset->trust);
		val->dsset = &val->frdataset;
		result = validatezonekey(val, ISC_TRUE);
		if (result != DNS_R_WAIT)
			validator_done(val, result);
	} else if (eresult == DNS_R_NXRRSET ||
		   eresult == DNS_R_NCACHENXRRSET)
	{
		validator_log(val, ISC_LOG_DEBUG(3),
			      "falling back to insecurity proof");
		val->attributes |= VALATTR_INSECURITY;
		result = proveunsecure(val, ISC_FALSE);
		if (result != DNS_R_WAIT)
			validator_done(val, result);
	} else {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "dsfetched: got %s",
			      isc_result_totext(eresult));
		if (eresult == ISC_R_CANCELED)
			validator_done(val, eresult);
		else
			validator_done(val, DNS_R_NOVALIDDS);
	}
	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy)
		destroy(val);
}

/*
 * XXX there's too much duplicated code here.
 */
static void
dsfetched2(isc_task_t *task, isc_event_t *event) {
	dns_fetchevent_t *devent;
	dns_validator_t *val;
	dns_rdataset_t *rdataset;
	dns_name_t *tname;
	isc_boolean_t want_destroy;
	isc_result_t result;
	isc_result_t eresult;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_FETCHDONE);
	devent = (dns_fetchevent_t *)event;
	val = devent->ev_arg;
	rdataset = &val->frdataset;
	eresult = devent->result;

	dns_resolver_destroyfetch(&val->fetch);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in dsfetched2");
	LOCK(&val->lock);
	if (eresult == DNS_R_NXRRSET || eresult == DNS_R_NCACHENXRRSET) {
		/*
		 * There is no DS.  If this is a delegation, we're done.
		 */
		tname = dns_fixedname_name(&devent->foundname);
		if (isdelegation(tname, &val->frdataset, eresult)) {
			val->event->rdataset->trust = dns_trust_answer;
			validator_done(val, ISC_R_SUCCESS);
		} else {
			result = proveunsecure(val, ISC_TRUE);
			if (result != DNS_R_WAIT)
				validator_done(val, result);
		}
	} else if (eresult == ISC_R_SUCCESS ||
		   eresult == DNS_R_NXDOMAIN ||
		   eresult == DNS_R_NCACHENXDOMAIN)
	{
		/*
		 * Either there is a DS or this is not a zone cut.  Continue.
		 */
		result = proveunsecure(val, ISC_TRUE);
		if (result != DNS_R_WAIT)
			validator_done(val, result);
	} else {
		if (eresult == ISC_R_CANCELED)
			validator_done(val, eresult);
		else
			validator_done(val, DNS_R_NOVALIDDS);
	}
	isc_event_free(&event);
	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy)
		destroy(val);
}

static void
keyvalidated(isc_task_t *task, isc_event_t *event) {
	dns_validatorevent_t *devent;
	dns_validator_t *val;
	isc_boolean_t want_destroy;
	isc_result_t result;
	isc_result_t eresult;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_VALIDATORDONE);

	devent = (dns_validatorevent_t *)event;
	val = devent->ev_arg;
	eresult = devent->result;

	isc_event_free(&event);
	dns_validator_destroy(&val->subvalidator);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in keyvalidated");
	LOCK(&val->lock);
	if (eresult == ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "keyset with trust %d", val->frdataset.trust);
		/*
		 * Only extract the dst key if the keyset is secure.
		 */
		if (val->frdataset.trust >= dns_trust_secure)
			(void) get_dst_key(val, val->siginfo, &val->frdataset);
		result = validate(val, ISC_TRUE);
		if (result != DNS_R_WAIT)
			validator_done(val, result);
	} else {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "keyvalidated: got %s",
			      isc_result_totext(eresult));
		validator_done(val, eresult);
	}
	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy)
		destroy(val);
}

static void
dsvalidated(isc_task_t *task, isc_event_t *event) {
	dns_validatorevent_t *devent;
	dns_validator_t *val;
	isc_boolean_t want_destroy;
	isc_result_t result;
	isc_result_t eresult;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_VALIDATORDONE);

	devent = (dns_validatorevent_t *)event;
	val = devent->ev_arg;
	eresult = devent->result;

	isc_event_free(&event);
	dns_validator_destroy(&val->subvalidator);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in dsvalidated");
	LOCK(&val->lock);
	if (eresult == ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "dsset with trust %d", val->frdataset.trust);
		if ((val->attributes & VALATTR_INSECURITY) != 0)
			result = proveunsecure(val, ISC_TRUE);
		else
			result = validatezonekey(val, ISC_TRUE);
		if (result != DNS_R_WAIT)
			validator_done(val, result);
	} else {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "dsvalidated: got %s",
			      isc_result_totext(eresult));
		validator_done(val, eresult);
	}
	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy)
		destroy(val);
}

/*
 * Return ISC_R_SUCCESS if we can determine that the name doesn't exist
 * or we can determine whether there is data or not at the name.
 * If the name does not exist return the wildcard name.
 */
static isc_result_t
nsecnoexistnodata(dns_validator_t *val, dns_name_t* name, dns_name_t *nsecname,
		  dns_rdataset_t *nsecset, isc_boolean_t *exists,
		  isc_boolean_t *data, dns_name_t *wild)
{
	int order;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	isc_result_t result;
	dns_namereln_t relation;
	unsigned int olabels, nlabels, labels;
	dns_rdata_nsec_t nsec;
	isc_boolean_t atparent;

	REQUIRE(exists != NULL);
	REQUIRE(data != NULL);

	result = dns_rdataset_first(nsecset);
	if (result != ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			"failure processing NSEC set");
		return (result);
	}
	dns_rdataset_current(nsecset, &rdata);

	validator_log(val, ISC_LOG_DEBUG(3), "looking for relevant nsec");
	relation = dns_name_fullcompare(name, nsecname, &order, &olabels);

	if (order < 0) {
		/*
		 * The name is not within the NSEC range.
		 */
		validator_log(val, ISC_LOG_DEBUG(3),
			      "NSEC does not cover name, before NSEC");
		return (ISC_R_IGNORE);
	}

	if (order == 0) {
		/*
		 * The names are the same.
		 */
		atparent = dns_rdatatype_atparent(val->event->type);
		if (dns_nsec_typepresent(&rdata, dns_rdatatype_ns) &&
		    !dns_nsec_typepresent(&rdata, dns_rdatatype_soa))
		{
			if (!atparent) {
				/*
				 * This NSEC record is from somewhere higher in
				 * the DNS, and at the parent of a delegation.
				 * It can not be legitimately used here.
				 */
				validator_log(val, ISC_LOG_DEBUG(3),
					      "ignoring parent nsec");
				return (ISC_R_IGNORE);
			}
		} else if (atparent) {
			/*
			 * This NSEC record is from the child.
			 * It can not be legitimately used here.
			 */
			validator_log(val, ISC_LOG_DEBUG(3),
				      "ignoring child nsec");
			return (ISC_R_IGNORE);
		}
		*exists = ISC_TRUE;
		*data = dns_nsec_typepresent(&rdata, val->event->type);
		validator_log(val, ISC_LOG_DEBUG(3),
			      "nsec proves name exists (owner) data=%d",
			      *data);
		return (ISC_R_SUCCESS);
	} 

	if (relation == dns_namereln_subdomain &&
	    dns_nsec_typepresent(&rdata, dns_rdatatype_ns) &&
	    !dns_nsec_typepresent(&rdata, dns_rdatatype_soa))
	{
		/*
		 * This NSEC record is from somewhere higher in
		 * the DNS, and at the parent of a delegation.
		 * It can not be legitimately used here.
		 */
		validator_log(val, ISC_LOG_DEBUG(3), "ignoring parent nsec");
		return (ISC_R_IGNORE);
	}

	result = dns_rdata_tostruct(&rdata, &nsec, NULL);
	if (result != ISC_R_SUCCESS)
		return (result);
	relation = dns_name_fullcompare(&nsec.next, name, &order, &nlabels);
	if (order == 0) {
		dns_rdata_freestruct(&nsec);
		validator_log(val, ISC_LOG_DEBUG(3),
			      "ignoring nsec matches next name");
		return (ISC_R_IGNORE);
	}

	if (order < 0 && !dns_name_issubdomain(nsecname, &nsec.next)) {
		/*
		 * The name is not within the NSEC range.
		 */
		dns_rdata_freestruct(&nsec);
		validator_log(val, ISC_LOG_DEBUG(3),
			    "ignoring nsec because name is past end of range");
		return (ISC_R_IGNORE);
	}

	if (order > 0 && relation == dns_namereln_subdomain) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "nsec proves name exist (empty)");
		dns_rdata_freestruct(&nsec);
		*exists = ISC_TRUE;
		*data = ISC_FALSE;
		return (ISC_R_SUCCESS);
	}
	if (wild != NULL) {
		dns_name_t common;
		dns_name_init(&common, NULL);
		if (olabels > nlabels) {
			labels = dns_name_countlabels(nsecname);
			dns_name_getlabelsequence(nsecname, labels - olabels,
						  olabels, &common);
		} else {
			labels = dns_name_countlabels(&nsec.next);
			dns_name_getlabelsequence(&nsec.next, labels - nlabels,
						  nlabels, &common);
		}
		result = dns_name_concatenate(dns_wildcardname, &common,
					       wild, NULL);
		if (result != ISC_R_SUCCESS) {
			validator_log(val, ISC_LOG_DEBUG(3),
				    "failure generating wilcard name");
			return (result);
		}
	}
	dns_rdata_freestruct(&nsec);
	validator_log(val, ISC_LOG_DEBUG(3), "nsec range ok");
	*exists = ISC_FALSE;
	return (ISC_R_SUCCESS);
}

static void
authvalidated(isc_task_t *task, isc_event_t *event) {
	dns_validatorevent_t *devent;
	dns_validator_t *val;
	dns_rdataset_t *rdataset, *sigrdataset;
	isc_boolean_t want_destroy;
	isc_result_t result;
	isc_boolean_t exists, data;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_VALIDATORDONE);

	devent = (dns_validatorevent_t *)event;
	rdataset = devent->rdataset;
	sigrdataset = devent->sigrdataset;
	val = devent->ev_arg;
	result = devent->result;
	dns_validator_destroy(&val->subvalidator);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in authvalidated");
	LOCK(&val->lock);
	if (result != ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "authvalidated: got %s",
			      isc_result_totext(result));
		if (result == ISC_R_CANCELED)
			validator_done(val, result);
		else {
			result = nsecvalidate(val, ISC_TRUE);
			if (result != DNS_R_WAIT)
				validator_done(val, result);
		}
	} else {
		dns_name_t **proofs = val->event->proofs;
		
		if (rdataset->trust == dns_trust_secure)
			val->seensig = ISC_TRUE;

		if (val->nsecset != NULL &&
		    rdataset->trust == dns_trust_secure &&
		    ((val->attributes & VALATTR_NEEDNODATA) != 0 ||
		     (val->attributes & VALATTR_NEEDNOQNAME) != 0) &&
	            (val->attributes & VALATTR_FOUNDNODATA) == 0 &&
		    (val->attributes & VALATTR_FOUNDNOQNAME) == 0 &&
		    nsecnoexistnodata(val, val->event->name, devent->name,
				      rdataset, &exists, &data,
				      dns_fixedname_name(&val->wild))
				      == ISC_R_SUCCESS)
		 {
			if (exists && !data) {
				val->attributes |= VALATTR_FOUNDNODATA;
				if (NEEDNODATA(val))
					proofs[DNS_VALIDATOR_NODATAPROOF] =
						devent->name;
			}
			if (!exists) {
				val->attributes |= VALATTR_FOUNDNOQNAME;
				if (NEEDNOQNAME(val))
					proofs[DNS_VALIDATOR_NOQNAMEPROOF] =
						devent->name;
			}
		}
		result = nsecvalidate(val, ISC_TRUE);
		if (result != DNS_R_WAIT)
			validator_done(val, result);
	}
	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy)
		destroy(val);

	/*
	 * Free stuff from the event.
	 */
	isc_event_free(&event);
}

static void
negauthvalidated(isc_task_t *task, isc_event_t *event) {
	dns_validatorevent_t *devent;
	dns_validator_t *val;
	isc_boolean_t want_destroy;
	isc_result_t eresult;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_VALIDATORDONE);

	devent = (dns_validatorevent_t *)event;
	val = devent->ev_arg;
	eresult = devent->result;
	isc_event_free(&event);
	dns_validator_destroy(&val->subvalidator);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in negauthvalidated");
	LOCK(&val->lock);
	if (eresult == ISC_R_SUCCESS) {
		val->attributes |= VALATTR_FOUNDNONEXISTENCE;
		validator_log(val, ISC_LOG_DEBUG(3),
			      "nonexistence proof found");
		auth_nonpending(val->event->message);
		validator_done(val, ISC_R_SUCCESS);
	} else {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "negauthvalidated: got %s",
			      isc_result_totext(eresult));
		validator_done(val, eresult);
	}
	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy)
		destroy(val);
}

static inline isc_result_t
view_find(dns_validator_t *val, dns_name_t *name, dns_rdatatype_t type) {
	if (dns_rdataset_isassociated(&val->frdataset))
		dns_rdataset_disassociate(&val->frdataset);
	if (dns_rdataset_isassociated(&val->fsigrdataset))
		dns_rdataset_disassociate(&val->fsigrdataset);

	if (val->view->zonetable == NULL)
		return (ISC_R_CANCELED);
	return (dns_view_simplefind(val->view, name, type, 0,
				    DNS_DBFIND_PENDINGOK, ISC_FALSE,
				    &val->frdataset, &val->fsigrdataset));
}

static inline isc_boolean_t
check_deadlock(dns_validator_t *val, dns_name_t *name, dns_rdatatype_t type) {
	dns_validator_t *parent;

	for (parent = val->parent; parent != NULL; parent = parent->parent) {
		if (parent->event != NULL &&
		    parent->event->type == type &&
		    dns_name_equal(parent->event->name, name))
		{
			validator_log(val, ISC_LOG_DEBUG(3),
				      "continuing validation would lead to "
				      "deadlock: aborting validation");
			return (ISC_TRUE);
		}
	}
	return (ISC_FALSE);
}

static inline isc_result_t
create_fetch(dns_validator_t *val, dns_name_t *name, dns_rdatatype_t type,
	     isc_taskaction_t callback, const char *caller)
{
	if (dns_rdataset_isassociated(&val->frdataset))
		dns_rdataset_disassociate(&val->frdataset);
	if (dns_rdataset_isassociated(&val->fsigrdataset))
		dns_rdataset_disassociate(&val->fsigrdataset);

	if (check_deadlock(val, name, type))
		return (DNS_R_NOVALIDSIG);

	validator_logcreate(val, name, type, caller, "fetch");
	return (dns_resolver_createfetch(val->view->resolver, name, type,
					 NULL, NULL, NULL, 0,
					 val->event->ev_sender,
					 callback, val,
					 &val->frdataset,
					 &val->fsigrdataset,
					 &val->fetch));
}

static inline isc_result_t
create_validator(dns_validator_t *val, dns_name_t *name, dns_rdatatype_t type,
		 dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset,
		 isc_taskaction_t action, const char *caller)
{
	isc_result_t result;

	if (check_deadlock(val, name, type))
		return (DNS_R_NOVALIDSIG);

	validator_logcreate(val, name, type, caller, "validator");
	result = dns_validator_create(val->view, name, type,
				      rdataset, sigrdataset, NULL, 0,
				      val->task, action, val,
				      &val->subvalidator);
	if (result == ISC_R_SUCCESS)
		val->subvalidator->parent = val;
	return (result);
}

/*
 * Try to find a key that could have signed 'siginfo' among those
 * in 'rdataset'.  If found, build a dst_key_t for it and point
 * val->key at it.
 *
 * If val->key is non-NULL, this returns the next matching key.
 */
static isc_result_t
get_dst_key(dns_validator_t *val, dns_rdata_rrsig_t *siginfo,
	    dns_rdataset_t *rdataset)
{
	isc_result_t result;
	isc_buffer_t b;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dst_key_t *oldkey = val->key;
	isc_boolean_t foundold;

	if (oldkey == NULL)
		foundold = ISC_TRUE;
	else {
		foundold = ISC_FALSE;
		val->key = NULL;
	}

	result = dns_rdataset_first(rdataset);
	if (result != ISC_R_SUCCESS)
		goto failure;
	do {
		dns_rdataset_current(rdataset, &rdata);

		isc_buffer_init(&b, rdata.data, rdata.length);
		isc_buffer_add(&b, rdata.length);
		INSIST(val->key == NULL);
		result = dst_key_fromdns(&siginfo->signer, rdata.rdclass, &b,
					 val->view->mctx, &val->key);
		if (result != ISC_R_SUCCESS)
			goto failure;
		if (siginfo->algorithm ==
		    (dns_secalg_t)dst_key_alg(val->key) &&
		    siginfo->keyid ==
		    (dns_keytag_t)dst_key_id(val->key) &&
		    dst_key_iszonekey(val->key))
		{
			if (foundold)
				/*
				 * This is the key we're looking for.
				 */
				return (ISC_R_SUCCESS);
			else if (dst_key_compare(oldkey, val->key) == ISC_TRUE)
			{
				foundold = ISC_TRUE;
				dst_key_free(&oldkey);
			}
		}
		dst_key_free(&val->key);
		dns_rdata_reset(&rdata);
		result = dns_rdataset_next(rdataset);
	} while (result == ISC_R_SUCCESS);
	if (result == ISC_R_NOMORE)
		result = ISC_R_NOTFOUND;

 failure:
	if (oldkey != NULL)
		dst_key_free(&oldkey);

	return (result);
}

static isc_result_t
get_key(dns_validator_t *val, dns_rdata_rrsig_t *siginfo) {
	isc_result_t result;
	unsigned int nlabels;
	int order;
	dns_namereln_t namereln;

	/*
	 * Is the signer name appropriate for this signature?
	 *
	 * The signer name must be at the same level as the owner name
	 * or closer to the the DNS root.
	 */
	namereln = dns_name_fullcompare(val->event->name, &siginfo->signer,
					&order, &nlabels);
	if (namereln != dns_namereln_subdomain &&
	    namereln != dns_namereln_equal)
		return (DNS_R_CONTINUE);

	if (namereln == dns_namereln_equal) {
		/*
		 * If this is a self-signed keyset, it must not be a zone key
		 * (since get_key is not called from validatezonekey).
		 */
		if (val->event->rdataset->type == dns_rdatatype_dnskey)
			return (DNS_R_CONTINUE);

		/*
		 * Records appearing in the parent zone at delegation
		 * points cannot be self-signed.
		 */
		if (dns_rdatatype_atparent(val->event->rdataset->type))
			return (DNS_R_CONTINUE);
	}

	/*
	 * Do we know about this key?
	 */
	result = view_find(val, &siginfo->signer, dns_rdatatype_dnskey);
	if (result == ISC_R_SUCCESS) {
		/*
		 * We have an rrset for the given keyname.
		 */
		val->keyset = &val->frdataset;
		if (val->frdataset.trust == dns_trust_pending &&
		    dns_rdataset_isassociated(&val->fsigrdataset))
		{
			/*
			 * We know the key but haven't validated it yet.
			 */
			result = create_validator(val, &siginfo->signer,
						  dns_rdatatype_dnskey,
						  &val->frdataset,
						  &val->fsigrdataset,
						  keyvalidated,
						  "get_key");
			if (result != ISC_R_SUCCESS)
				return (result);
			return (DNS_R_WAIT);
		} else if (val->frdataset.trust == dns_trust_pending) {
			/*
			 * Having a pending key with no signature means that
			 * something is broken.
			 */
			result = DNS_R_CONTINUE;
		} else if (val->frdataset.trust < dns_trust_secure) {
			/*
			 * The key is legitimately insecure.  There's no
			 * point in even attempting verification.
			 */
			val->key = NULL;
			result = ISC_R_SUCCESS;
		} else {
			/*
			 * See if we've got the key used in the signature.
			 */
			validator_log(val, ISC_LOG_DEBUG(3),
				      "keyset with trust %d",
				      val->frdataset.trust);
			result = get_dst_key(val, siginfo, val->keyset);
			if (result != ISC_R_SUCCESS) {
				/*
				 * Either the key we're looking for is not
				 * in the rrset, or something bad happened.
				 * Give up.
				 */
				result = DNS_R_CONTINUE;
			}
		}
	} else if (result == ISC_R_NOTFOUND) {
		/*
		 * We don't know anything about this key.
		 */
		result = create_fetch(val, &siginfo->signer, dns_rdatatype_dnskey,
				      fetch_callback_validator, "get_key");
		if (result != ISC_R_SUCCESS)
			return (result);
		return (DNS_R_WAIT);
	} else if (result ==  DNS_R_NCACHENXDOMAIN ||
		   result == DNS_R_NCACHENXRRSET ||
		   result == DNS_R_NXDOMAIN ||
		   result == DNS_R_NXRRSET)
	{
		/*
		 * This key doesn't exist.
		 */
		result = DNS_R_CONTINUE;
	}

	if (dns_rdataset_isassociated(&val->frdataset) &&
	    val->keyset != &val->frdataset)
		dns_rdataset_disassociate(&val->frdataset);
	if (dns_rdataset_isassociated(&val->fsigrdataset))
		dns_rdataset_disassociate(&val->fsigrdataset);

	return (result);
}

static dns_keytag_t
compute_keytag(dns_rdata_t *rdata, dns_rdata_dnskey_t *key) {
	isc_region_t r;

	dns_rdata_toregion(rdata, &r);
	return (dst_region_computeid(&r, key->algorithm));
}

/*
 * Is this keyset self-signed?
 */
static isc_boolean_t
isselfsigned(dns_validator_t *val) {
	dns_rdataset_t *rdataset, *sigrdataset;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdata_t sigrdata = DNS_RDATA_INIT;
	dns_rdata_dnskey_t key;
	dns_rdata_rrsig_t sig;
	dns_keytag_t keytag;
	isc_result_t result;

	rdataset = val->event->rdataset;
	sigrdataset = val->event->sigrdataset;

	INSIST(rdataset->type == dns_rdatatype_dnskey);

	for (result = dns_rdataset_first(rdataset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(rdataset))
	{
		dns_rdata_reset(&rdata);
		dns_rdataset_current(rdataset, &rdata);
		(void)dns_rdata_tostruct(&rdata, &key, NULL);
		keytag = compute_keytag(&rdata, &key);
		for (result = dns_rdataset_first(sigrdataset);
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(sigrdataset))
		{
			dns_rdata_reset(&sigrdata);
			dns_rdataset_current(sigrdataset, &sigrdata);
			(void)dns_rdata_tostruct(&sigrdata, &sig, NULL);

			if (sig.algorithm == key.algorithm &&
			    sig.keyid == keytag)
				return (ISC_TRUE);
		}
	}
	return (ISC_FALSE);
}

static isc_result_t
verify(dns_validator_t *val, dst_key_t *key, dns_rdata_t *rdata) {
	isc_result_t result;
	dns_fixedname_t fixed;

	val->attributes |= VALATTR_TRIEDVERIFY;
	dns_fixedname_init(&fixed);
	result = dns_dnssec_verify2(val->event->name, val->event->rdataset,
				    key, ISC_FALSE, val->view->mctx, rdata,
				    dns_fixedname_name(&fixed));
	validator_log(val, ISC_LOG_DEBUG(3),
		      "verify rdataset: %s",
		      isc_result_totext(result));
	if (result == DNS_R_FROMWILDCARD) {
		if (!dns_name_equal(val->event->name,
				    dns_fixedname_name(&fixed)))
			val->attributes |= VALATTR_NEEDNOQNAME;
		result = ISC_R_SUCCESS;
	}
	return (result);
}

/*
 * Attempts positive response validation of a normal RRset.
 *
 * Returns:
 *	ISC_R_SUCCESS	Validation completed successfully
 *	DNS_R_WAIT	Validation has started but is waiting
 *			for an event.
 *	Other return codes are possible and all indicate failure.
 */
static isc_result_t
validate(dns_validator_t *val, isc_boolean_t resume) {
	isc_result_t result;
	dns_validatorevent_t *event;
	dns_rdata_t rdata = DNS_RDATA_INIT;

	/*
	 * Caller must be holding the validator lock.
	 */

	event = val->event;

	if (resume) {
		/*
		 * We already have a sigrdataset.
		 */
		result = ISC_R_SUCCESS;
		validator_log(val, ISC_LOG_DEBUG(3), "resuming validate");
	} else {
		result = dns_rdataset_first(event->sigrdataset);
	}

	for (;
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(event->sigrdataset))
	{
		dns_rdata_reset(&rdata);
		dns_rdataset_current(event->sigrdataset, &rdata);
		if (val->siginfo == NULL) {
			val->siginfo = isc_mem_get(val->view->mctx,
						   sizeof(*val->siginfo));
			if (val->siginfo == NULL)
				return (ISC_R_NOMEMORY);
		}
		result = dns_rdata_tostruct(&rdata, val->siginfo, NULL);
		if (result != ISC_R_SUCCESS)
			return (result);

		/*
		 * At this point we could check that the signature algorithm
		 * was known and "sufficiently good".
		 */
		if (!dns_resolver_algorithm_supported(val->view->resolver,
						      event->name,
						      val->siginfo->algorithm))
			continue;

		if (!resume) {
			result = get_key(val, val->siginfo);
			if (result == DNS_R_CONTINUE)
				continue; /* Try the next SIG RR. */
			if (result != ISC_R_SUCCESS)
				return (result);
		}

		/*
		 * The key is insecure, so mark the data as insecure also.
		 */
		if (val->key == NULL) {
			event->rdataset->trust = dns_trust_answer;
			event->sigrdataset->trust = dns_trust_answer;
			validator_log(val, ISC_LOG_DEBUG(3),
				      "marking as answer");
			return (ISC_R_SUCCESS);
		}

		do {
			result = verify(val, val->key, &rdata);
			if (result == ISC_R_SUCCESS)
				break;
			if (val->keynode != NULL) {
				dns_keynode_t *nextnode = NULL;
				result = dns_keytable_findnextkeynode(
							val->keytable,
							val->keynode,
							&nextnode);
				dns_keytable_detachkeynode(val->keytable,
							   &val->keynode);
				val->keynode = nextnode;
				if (result != ISC_R_SUCCESS) {
					val->key = NULL;
					break;
				}
				val->key = dns_keynode_key(val->keynode);
			} else {
				if (get_dst_key(val, val->siginfo, val->keyset)
				    != ISC_R_SUCCESS)
					break;
			}
		} while (1);
		if (result != ISC_R_SUCCESS)
			validator_log(val, ISC_LOG_DEBUG(3),
				      "failed to verify rdataset");
		else {
			isc_uint32_t ttl;
			isc_stdtime_t now;

			isc_stdtime_get(&now);
			ttl = ISC_MIN(event->rdataset->ttl,
				      val->siginfo->timeexpire - now);
			if (val->keyset != NULL)
				ttl = ISC_MIN(ttl, val->keyset->ttl);
			event->rdataset->ttl = ttl;
			event->sigrdataset->ttl = ttl;
		}

		if (val->keynode != NULL)
			dns_keytable_detachkeynode(val->keytable,
						   &val->keynode);
		else {
			if (val->key != NULL)
				dst_key_free(&val->key);
			if (val->keyset != NULL) {
				dns_rdataset_disassociate(val->keyset);
				val->keyset = NULL;
			}
		}
		val->key = NULL;
		if ((val->attributes & VALATTR_NEEDNOQNAME) != 0) {
			if (val->event->message == NULL) {
				validator_log(val, ISC_LOG_DEBUG(3),
				      "no message available for noqname proof");
				return (DNS_R_NOVALIDSIG);
			}
			validator_log(val, ISC_LOG_DEBUG(3),
				      "looking for noqname proof");
			return (nsecvalidate(val, ISC_FALSE));
		} else if (result == ISC_R_SUCCESS) {
			event->rdataset->trust = dns_trust_secure;
			event->sigrdataset->trust = dns_trust_secure;
			validator_log(val, ISC_LOG_DEBUG(3),
				      "marking as secure");
			return (result);
		} else {
			validator_log(val, ISC_LOG_DEBUG(3),
				      "verify failure: %s",
				      isc_result_totext(result));
			resume = ISC_FALSE;
		}
	}
	if (result != ISC_R_NOMORE) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "failed to iterate signatures: %s",
			      isc_result_totext(result));
		return (result);
	}

	validator_log(val, ISC_LOG_INFO, "no valid signature found");
	return (DNS_R_NOVALIDSIG);
}

/*
 * Attempts positive response validation of an RRset containing zone keys.
 *
 * Returns:
 *	ISC_R_SUCCESS	Validation completed successfully
 *	DNS_R_WAIT	Validation has started but is waiting
 *			for an event.
 *	Other return codes are possible and all indicate failure.
 */
static isc_result_t
validatezonekey(dns_validator_t *val, isc_boolean_t resume) {
	isc_result_t result;
	dns_validatorevent_t *event;
	dns_rdataset_t trdataset;
	dns_rdata_t dsrdata = DNS_RDATA_INIT;
	dns_rdata_t newdsrdata = DNS_RDATA_INIT;
	dns_rdata_t keyrdata = DNS_RDATA_INIT;
	dns_rdata_t sigrdata = DNS_RDATA_INIT;
	unsigned char dsbuf[DNS_DS_BUFFERSIZE];
	dns_keytag_t keytag;
	dns_rdata_ds_t ds;
	dns_rdata_dnskey_t key;
	dns_rdata_rrsig_t sig;
	dst_key_t *dstkey;
	isc_boolean_t supported_algorithm;

	UNUSED(resume);

	/*
	 * Caller must be holding the validator lock.
	 */

	event = val->event;

	if (val->dsset == NULL) {
		/*
		 * First, see if this key was signed by a trusted key.
		 */
		for (result = dns_rdataset_first(val->event->sigrdataset);
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(val->event->sigrdataset))
		{
			dns_keynode_t *keynode = NULL, *nextnode = NULL;

			dns_rdata_reset(&sigrdata);
			dns_rdataset_current(val->event->sigrdataset,
					     &sigrdata);
			(void)dns_rdata_tostruct(&sigrdata, &sig, NULL);
			result = dns_keytable_findkeynode(val->keytable,
							  val->event->name,
							  sig.algorithm,
							  sig.keyid,
							  &keynode);
			while (result == ISC_R_SUCCESS) {
				dstkey = dns_keynode_key(keynode);
				result = verify(val, dstkey, &sigrdata);
				if (result == ISC_R_SUCCESS) {
					dns_keytable_detachkeynode(val->keytable,
								   &keynode);
					break;
				}
				result = dns_keytable_findnextkeynode(
								val->keytable,
								keynode,
								&nextnode);
				dns_keytable_detachkeynode(val->keytable,
							   &keynode);
				keynode = nextnode;
			}
			if (result == ISC_R_SUCCESS) {
				event->rdataset->trust = dns_trust_secure;
				event->sigrdataset->trust = dns_trust_secure;
				validator_log(val, ISC_LOG_DEBUG(3),
					      "signed by trusted key; "
					      "marking as secure");
				return (result);
			}
		}

		/*
		 * If this is the root name and there was no trusted key,
		 * give up, since there's no DS at the root.
		 */
		if (dns_name_equal(event->name, dns_rootname)) {
			if ((val->attributes & VALATTR_TRIEDVERIFY) != 0)
				return (DNS_R_NOVALIDSIG);
			else
				return (DNS_R_NOVALIDDS);
		}

		/*
		 * Otherwise, try to find the DS record.
		 */
		result = view_find(val, val->event->name, dns_rdatatype_ds);
		if (result == ISC_R_SUCCESS) {
			/*
			 * We have DS records.
			 */
			val->dsset = &val->frdataset;
			if (val->frdataset.trust == dns_trust_pending &&
			    dns_rdataset_isassociated(&val->fsigrdataset))
			{
				result = create_validator(val,
							  val->event->name,
							  dns_rdatatype_ds,
							  &val->frdataset,
							  &val->fsigrdataset,
							  dsvalidated,
							  "validatezonekey");
				if (result != ISC_R_SUCCESS)
					return (result);
				return (DNS_R_WAIT);
			} else if (val->frdataset.trust == dns_trust_pending) {
				/*
				 * There should never be an unsigned DS.
				 */
				dns_rdataset_disassociate(&val->frdataset);
				validator_log(val, ISC_LOG_DEBUG(2),
					      "unsigned DS record");
				return (DNS_R_NOVALIDSIG);
			} else
				result = ISC_R_SUCCESS;
		} else if (result == ISC_R_NOTFOUND) {
			/*
			 * We don't have the DS.  Find it.
			 */
			result = create_fetch(val, val->event->name,
					      dns_rdatatype_ds, dsfetched,
					      "validatezonekey");
			if (result != ISC_R_SUCCESS)
				return (result);
			return (DNS_R_WAIT);
		} else if (result ==  DNS_R_NCACHENXDOMAIN ||
			   result == DNS_R_NCACHENXRRSET ||
			   result == DNS_R_NXDOMAIN ||
			   result == DNS_R_NXRRSET)
		{
			/*
			 * The DS does not exist.
			 */
			if (dns_rdataset_isassociated(&val->frdataset))
				dns_rdataset_disassociate(&val->frdataset);
			if (dns_rdataset_isassociated(&val->fsigrdataset))
				dns_rdataset_disassociate(&val->fsigrdataset);
			validator_log(val, ISC_LOG_DEBUG(2), "no DS record");
			return (DNS_R_NOVALIDSIG);
		}
	}

	/*
	 * We have a DS set.
	 */
	INSIST(val->dsset != NULL);

	if (val->dsset->trust < dns_trust_secure) {
		val->event->rdataset->trust = dns_trust_answer;
		val->event->sigrdataset->trust = dns_trust_answer;
		return (ISC_R_SUCCESS);
	}

	/*
	 * Look through the DS record and find the keys that can sign the
	 * key set and the matching signature.  For each such key, attempt
	 * verification.
	 */

	supported_algorithm = ISC_FALSE;

	for (result = dns_rdataset_first(val->dsset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(val->dsset))
	{
		dns_rdata_reset(&dsrdata);
		dns_rdataset_current(val->dsset, &dsrdata);
		(void)dns_rdata_tostruct(&dsrdata, &ds, NULL);

		if (!dns_resolver_algorithm_supported(val->view->resolver,
						      val->event->name,
						      ds.algorithm))
			continue;

		supported_algorithm = ISC_TRUE;

		dns_rdataset_init(&trdataset);
		dns_rdataset_clone(val->event->rdataset, &trdataset);

		for (result = dns_rdataset_first(&trdataset);
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(&trdataset))
		{
			dns_rdata_reset(&keyrdata);
			dns_rdataset_current(&trdataset, &keyrdata);
			(void)dns_rdata_tostruct(&keyrdata, &key, NULL);
			keytag = compute_keytag(&keyrdata, &key);
			if (ds.key_tag != keytag ||
			    ds.algorithm != key.algorithm)
				continue;
			dns_rdata_reset(&newdsrdata);
			result = dns_ds_buildrdata(val->event->name,
						   &keyrdata, ds.digest_type,
						   dsbuf, &newdsrdata);
			if (result != ISC_R_SUCCESS)
				continue;
			if (dns_rdata_compare(&dsrdata, &newdsrdata) == 0)
				break;
		}
		if (result != ISC_R_SUCCESS) {
			validator_log(val, ISC_LOG_DEBUG(3),
				      "no KEY matching DS");
			continue;
		}
		
		for (result = dns_rdataset_first(val->event->sigrdataset);
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(val->event->sigrdataset))
		{
			dns_rdata_reset(&sigrdata);
			dns_rdataset_current(val->event->sigrdataset,
					     &sigrdata);
			(void)dns_rdata_tostruct(&sigrdata, &sig, NULL);
			if (ds.key_tag != sig.keyid &&
			    ds.algorithm != sig.algorithm)
				continue;

			dstkey = NULL;
			result = dns_dnssec_keyfromrdata(val->event->name,
							 &keyrdata,
							 val->view->mctx,
							 &dstkey);
			if (result != ISC_R_SUCCESS)
				/*
				 * This really shouldn't happen, but...
				 */
				continue;

			result = verify(val, dstkey, &sigrdata);
			dst_key_free(&dstkey);
			if (result == ISC_R_SUCCESS)
				break;
		}
		dns_rdataset_disassociate(&trdataset);
		if (result == ISC_R_SUCCESS)
			break;
		validator_log(val, ISC_LOG_DEBUG(3), "no SIG matching DS key");
	}
	if (result == ISC_R_SUCCESS) {
		event->rdataset->trust = dns_trust_secure;
		event->sigrdataset->trust = dns_trust_secure;
		validator_log(val, ISC_LOG_DEBUG(3), "marking as secure");
		return (result);
	} else if (result == ISC_R_NOMORE && !supported_algorithm) {
		val->event->rdataset->trust = dns_trust_answer;
		val->event->sigrdataset->trust = dns_trust_answer;
		validator_log(val, ISC_LOG_DEBUG(3),
			      "no supported algorithm (ds)");
		return (ISC_R_SUCCESS);
	} else
		return (DNS_R_NOVALIDSIG);
}

/*
 * Starts a positive response validation.
 *
 * Returns:
 *	ISC_R_SUCCESS	Validation completed successfully
 *	DNS_R_WAIT	Validation has started but is waiting
 *			for an event.
 *	Other return codes are possible and all indicate failure.
 */
static isc_result_t
start_positive_validation(dns_validator_t *val) {
	/*
	 * If this is not a key, go straight into validate().
	 */
	if (val->event->type != dns_rdatatype_dnskey || !isselfsigned(val))
		return (validate(val, ISC_FALSE));

	return (validatezonekey(val, ISC_FALSE));
}

static isc_result_t
checkwildcard(dns_validator_t *val) {
	dns_name_t *name, *wild;
	dns_message_t *message = val->event->message;
	isc_result_t result;
	isc_boolean_t exists, data;
	char namebuf[DNS_NAME_FORMATSIZE];

	wild = dns_fixedname_name(&val->wild);
	dns_name_format(wild, namebuf, sizeof(namebuf));
	validator_log(val, ISC_LOG_DEBUG(3), "in checkwildcard: %s", namebuf);

	for (result = dns_message_firstname(message, DNS_SECTION_AUTHORITY);
	     result == ISC_R_SUCCESS;
	     result = dns_message_nextname(message, DNS_SECTION_AUTHORITY))
	{
		dns_rdataset_t *rdataset = NULL, *sigrdataset = NULL;

		name = NULL;
		dns_message_currentname(message, DNS_SECTION_AUTHORITY, &name);

		for (rdataset = ISC_LIST_HEAD(name->list);
		     rdataset != NULL;
		     rdataset = ISC_LIST_NEXT(rdataset, link))
		{
			if (rdataset->type != dns_rdatatype_nsec)
				continue;
			val->nsecset = rdataset;

			for (sigrdataset = ISC_LIST_HEAD(name->list);
			     sigrdataset != NULL;
			     sigrdataset = ISC_LIST_NEXT(sigrdataset, link))
			{
				if (sigrdataset->type == dns_rdatatype_rrsig &&
				    sigrdataset->covers == rdataset->type)
					break;
			}
			if (sigrdataset == NULL)
				continue;

			if (rdataset->trust != dns_trust_secure)
				continue;

			if (((val->attributes & VALATTR_NEEDNODATA) != 0 ||
			     (val->attributes & VALATTR_NEEDNOWILDCARD) != 0) &&
			    (val->attributes & VALATTR_FOUNDNODATA) == 0 &&
			    (val->attributes & VALATTR_FOUNDNOWILDCARD) == 0 &&
			    nsecnoexistnodata(val, wild, name, rdataset,
					      &exists, &data, NULL)
					       == ISC_R_SUCCESS)
			{
				dns_name_t **proofs = val->event->proofs;
				if (exists && !data)
					val->attributes |= VALATTR_FOUNDNODATA;
				if (exists && !data && NEEDNODATA(val))
					proofs[DNS_VALIDATOR_NODATAPROOF] =
							 name;
				if (!exists)
					val->attributes |=
						 VALATTR_FOUNDNOWILDCARD;
				if (!exists && NEEDNOQNAME(val))
					proofs[DNS_VALIDATOR_NOWILDCARDPROOF] =
							 name;
				return (ISC_R_SUCCESS);
			}
		}
	}
	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;
	return (result);
}

static isc_result_t
nsecvalidate(dns_validator_t *val, isc_boolean_t resume) {
	dns_name_t *name;
	dns_message_t *message = val->event->message;
	isc_result_t result;

	if (!resume)
		result = dns_message_firstname(message, DNS_SECTION_AUTHORITY);
	else {
		result = ISC_R_SUCCESS;
		validator_log(val, ISC_LOG_DEBUG(3), "resuming nsecvalidate");
	}

	for (;
	     result == ISC_R_SUCCESS;
	     result = dns_message_nextname(message, DNS_SECTION_AUTHORITY))
	{
		dns_rdataset_t *rdataset = NULL, *sigrdataset = NULL;

		name = NULL;
		dns_message_currentname(message, DNS_SECTION_AUTHORITY, &name);
		if (resume) {
			rdataset = ISC_LIST_NEXT(val->currentset, link);
			val->currentset = NULL;
			resume = ISC_FALSE;
		} else
			rdataset = ISC_LIST_HEAD(name->list);

		for (;
		     rdataset != NULL;
		     rdataset = ISC_LIST_NEXT(rdataset, link))
		{
			if (rdataset->type == dns_rdatatype_rrsig)
				continue;

			if (rdataset->type == dns_rdatatype_soa) {
				val->soaset = rdataset;
				val->soaname = name;
			} else if (rdataset->type == dns_rdatatype_nsec)
				val->nsecset = rdataset;

			for (sigrdataset = ISC_LIST_HEAD(name->list);
			     sigrdataset != NULL;
			     sigrdataset = ISC_LIST_NEXT(sigrdataset,
							 link))
			{
				if (sigrdataset->type == dns_rdatatype_rrsig &&
				    sigrdataset->covers == rdataset->type)
					break;
			}
			if (sigrdataset == NULL)
				continue;
			/*
			 * If a signed zone is missing the zone key, bad
			 * things could happen.  A query for data in the zone
			 * would lead to a query for the zone key, which
			 * would return a negative answer, which would contain
			 * an SOA and an NSEC signed by the missing key, which
			 * would trigger another query for the KEY (since the
			 * first one is still in progress), and go into an
			 * infinite loop.  Avoid that.
			 */
			if (val->event->type == dns_rdatatype_dnskey &&
			    dns_name_equal(name, val->event->name))
			{
				dns_rdata_t nsec = DNS_RDATA_INIT;

				if (rdataset->type != dns_rdatatype_nsec)
					continue;

				result = dns_rdataset_first(rdataset);
				if (result != ISC_R_SUCCESS)
					return (result);
				dns_rdataset_current(rdataset, &nsec);
				if (dns_nsec_typepresent(&nsec,
							dns_rdatatype_soa))
					continue;
			}
			val->currentset = rdataset;
			result = create_validator(val, name, rdataset->type,
						  rdataset, sigrdataset,
						  authvalidated,
						  "nsecvalidate");
			if (result != ISC_R_SUCCESS)
				return (result);
			return (DNS_R_WAIT);

		}
	}
	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;
	if (result != ISC_R_SUCCESS)
		return (result);

	/*
	 * Do we only need to check for NOQNAME?
	 */
	if ((val->attributes & VALATTR_NEEDNODATA) == 0 &&
	    (val->attributes & VALATTR_NEEDNOWILDCARD) == 0 &&
	    (val->attributes & VALATTR_NEEDNOQNAME) != 0) {
		if ((val->attributes & VALATTR_FOUNDNOQNAME) != 0) {
			validator_log(val, ISC_LOG_DEBUG(3),
				      "noqname proof found");
			validator_log(val, ISC_LOG_DEBUG(3),
				      "marking as secure");
			val->event->rdataset->trust = dns_trust_secure;
			val->event->sigrdataset->trust = dns_trust_secure;
			return (ISC_R_SUCCESS);
		}
		validator_log(val, ISC_LOG_DEBUG(3),
			      "noqname proof not found");
		return (DNS_R_NOVALIDNSEC);
	}

	/*
	 * Do we need to check for the wildcard?
	 */
	if ((val->attributes & VALATTR_FOUNDNOQNAME) != 0 &&
	    (((val->attributes & VALATTR_NEEDNODATA) != 0 &&
	      (val->attributes & VALATTR_FOUNDNODATA) == 0) ||
	     (val->attributes & VALATTR_NEEDNOWILDCARD) != 0)) {
		result = checkwildcard(val);
		if (result != ISC_R_SUCCESS)
			return (result);
	}

	if (((val->attributes & VALATTR_NEEDNODATA) != 0 &&
	     (val->attributes & VALATTR_FOUNDNODATA) != 0) ||
	    ((val->attributes & VALATTR_NEEDNOQNAME) != 0 &&
	     (val->attributes & VALATTR_FOUNDNOQNAME) != 0 &&
	     (val->attributes & VALATTR_NEEDNOWILDCARD) != 0 &&
	     (val->attributes & VALATTR_FOUNDNOWILDCARD) != 0))
		val->attributes |= VALATTR_FOUNDNONEXISTENCE;

	if ((val->attributes & VALATTR_FOUNDNONEXISTENCE) == 0) {
		if (!val->seensig && val->soaset != NULL) {
			result = create_validator(val, name, dns_rdatatype_soa,
						  val->soaset, NULL,
						  negauthvalidated,
						  "nsecvalidate");
			if (result != ISC_R_SUCCESS)
				return (result);
			return (DNS_R_WAIT);
		}
		validator_log(val, ISC_LOG_DEBUG(3),
			      "nonexistence proof not found");
		return (DNS_R_NOVALIDNSEC);
	} else {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "nonexistence proof found");
		return (ISC_R_SUCCESS);
	}
}

static isc_boolean_t
check_ds_algorithm(dns_validator_t *val, dns_name_t *name,
		   dns_rdataset_t *rdataset) {
	dns_rdata_t dsrdata = DNS_RDATA_INIT;
	dns_rdata_ds_t ds;
	isc_result_t result;

	for (result = dns_rdataset_first(rdataset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(rdataset)) {
		dns_rdataset_current(rdataset, &dsrdata);
		(void)dns_rdata_tostruct(&dsrdata, &ds, NULL);

		if (dns_resolver_algorithm_supported(val->view->resolver,
						     name, ds.algorithm))
			return (ISC_TRUE);
		dns_rdata_reset(&dsrdata);
	}
	return (ISC_FALSE);
}

static isc_result_t
proveunsecure(dns_validator_t *val, isc_boolean_t resume) {
	isc_result_t result;
	dns_fixedname_t secroot;
	dns_name_t *tname;

	dns_fixedname_init(&secroot);
	result = dns_keytable_finddeepestmatch(val->keytable,
					       val->event->name,
					       dns_fixedname_name(&secroot));
	/*
	 * If the name is not under a security root, it must be insecure.
	 */
	if (result == ISC_R_NOTFOUND)
		return (ISC_R_SUCCESS);

	else if (result != ISC_R_SUCCESS)
		return (result);

	if (!resume) {
		val->labels =
			dns_name_countlabels(dns_fixedname_name(&secroot)) + 1;
	} else {
		validator_log(val, ISC_LOG_DEBUG(3), "resuming proveunsecure");
		if (val->frdataset.trust >= dns_trust_secure &&
		    !check_ds_algorithm(val, dns_fixedname_name(&val->fname),
					&val->frdataset)) {
			validator_log(val, ISC_LOG_DEBUG(3),
				      "no supported algorithm (ds)");
			val->event->rdataset->trust = dns_trust_answer;
			result = ISC_R_SUCCESS;
			goto out;
		}
		val->labels++;
	}

	for (;
	     val->labels <= dns_name_countlabels(val->event->name);
	     val->labels++)
	{
		char namebuf[DNS_NAME_FORMATSIZE];

		dns_fixedname_init(&val->fname);
		tname = dns_fixedname_name(&val->fname);
		if (val->labels == dns_name_countlabels(val->event->name))
			dns_name_copy(val->event->name, tname, NULL);
		else
			dns_name_split(val->event->name, val->labels,
				       NULL, tname);

		dns_name_format(tname, namebuf, sizeof(namebuf));
		validator_log(val, ISC_LOG_DEBUG(3),
			      "checking existence of DS at '%s'",
			      namebuf);

		result = view_find(val, tname, dns_rdatatype_ds);
		if (result == DNS_R_NXRRSET || result == DNS_R_NCACHENXRRSET) {
			/*
			 * There is no DS.  If this is a delegation,
			 * we're done.
			 */
			if (val->frdataset.trust < dns_trust_secure) {
				/*
				 * This shouldn't happen, since the negative
				 * response should have been validated.  Since
				 * there's no way of validating existing
				 * negative response blobs, give up.
				 */
				result = DNS_R_NOVALIDSIG;
				goto out;
			}
			if (isdelegation(tname, &val->frdataset, result)) {
				val->event->rdataset->trust = dns_trust_answer;
				return (ISC_R_SUCCESS);
			}
			continue;
		} else if (result == ISC_R_SUCCESS) {
			/*
			 * There is a DS here.  Verify that it's secure and
			 * continue.
			 */
			if (val->frdataset.trust >= dns_trust_secure) {
				if (!check_ds_algorithm(val, tname,
							&val->frdataset)) {
					validator_log(val, ISC_LOG_DEBUG(3),
					      "no supported algorithm (ds)");
					val->event->rdataset->trust =
							dns_trust_answer;
					result = ISC_R_SUCCESS;
					goto out;
				}
				continue;
			}
			else if (!dns_rdataset_isassociated(&val->fsigrdataset))
			{
				result = DNS_R_NOVALIDSIG;
				goto out;
			}
			result = create_validator(val, tname, dns_rdatatype_ds,
						  &val->frdataset,
						  &val->fsigrdataset,
						  dsvalidated,
						  "proveunsecure");
			if (result != ISC_R_SUCCESS)
				goto out;
			return (DNS_R_WAIT);
		} else if (result == DNS_R_NXDOMAIN ||
			   result == DNS_R_NCACHENXDOMAIN)
		{
			/*
			 * This is not a zone cut.  Assuming things are
			 * as expected, continue.
			 */
			if (!dns_rdataset_isassociated(&val->frdataset)) {
				/*
				 * There should be an NSEC here, since we
				 * are still in a secure zone.
				 */
				result = DNS_R_NOVALIDNSEC;
				goto out;
			} else if (val->frdataset.trust < dns_trust_secure) {
				/*
				 * This shouldn't happen, since the negative
				 * response should have been validated.  Since
				 * there's no way of validating existing
				 * negative response blobs, give up.
				 */
				result = DNS_R_NOVALIDSIG;
				goto out;
			}
			continue;
		} else if (result == ISC_R_NOTFOUND) {
			/*
			 * We don't know anything about the DS.  Find it.
			 */
			result = create_fetch(val, tname, dns_rdatatype_ds,
					      dsfetched2, "proveunsecure");
			if (result != ISC_R_SUCCESS)
				goto out;
			return (DNS_R_WAIT);
		}
	}
	validator_log(val, ISC_LOG_DEBUG(3), "insecurity proof failed");
	return (DNS_R_NOTINSECURE); /* Couldn't complete insecurity proof */

 out:
	if (dns_rdataset_isassociated(&val->frdataset))
		dns_rdataset_disassociate(&val->frdataset);
	if (dns_rdataset_isassociated(&val->fsigrdataset))
		dns_rdataset_disassociate(&val->fsigrdataset);
	return (result);
}

static void
validator_start(isc_task_t *task, isc_event_t *event) {
	dns_validator_t *val;
	dns_validatorevent_t *vevent;
	isc_boolean_t want_destroy = ISC_FALSE;
	isc_result_t result = ISC_R_FAILURE;

	UNUSED(task);
	REQUIRE(event->ev_type == DNS_EVENT_VALIDATORSTART);
	vevent = (dns_validatorevent_t *)event;
	val = vevent->validator;

	/* If the validator has been cancelled, val->event == NULL */
	if (val->event == NULL)
		return;

	validator_log(val, ISC_LOG_DEBUG(3), "starting");

	LOCK(&val->lock);

	if (val->event->rdataset != NULL && val->event->sigrdataset != NULL) {
		isc_result_t saved_result;

		/*
		 * This looks like a simple validation.  We say "looks like"
		 * because it might end up requiring an insecurity proof.
		 */
		validator_log(val, ISC_LOG_DEBUG(3),
			      "attempting positive response validation");

		INSIST(dns_rdataset_isassociated(val->event->rdataset));
		INSIST(dns_rdataset_isassociated(val->event->sigrdataset));
		result = start_positive_validation(val);
		if (result == DNS_R_NOVALIDSIG &&
		    (val->attributes & VALATTR_TRIEDVERIFY) == 0)
		{
			saved_result = result;
			validator_log(val, ISC_LOG_DEBUG(3),
				      "falling back to insecurity proof");
			val->attributes |= VALATTR_INSECURITY;
			result = proveunsecure(val, ISC_FALSE);
			if (result == DNS_R_NOTINSECURE)
				result = saved_result;
		}
	} else if (val->event->rdataset != NULL) {
		/*
		 * This is either an unsecure subdomain or a response from
		 * a broken server.
		 */
		INSIST(dns_rdataset_isassociated(val->event->rdataset));
		validator_log(val, ISC_LOG_DEBUG(3),
			      "attempting insecurity proof");

		val->attributes |= VALATTR_INSECURITY;
		result = proveunsecure(val, ISC_FALSE);
	} else if (val->event->rdataset == NULL &&
		   val->event->sigrdataset == NULL)
	{
		/*
		 * This is a nonexistence validation.
		 */
		validator_log(val, ISC_LOG_DEBUG(3),
			      "attempting negative response validation");

		val->attributes |= VALATTR_NEGATIVE;
		if (val->event->message->rcode == dns_rcode_nxdomain) {
			val->attributes |= VALATTR_NEEDNOQNAME;
			val->attributes |= VALATTR_NEEDNOWILDCARD;
		} else
			val->attributes |= VALATTR_NEEDNODATA;
		result = nsecvalidate(val, ISC_FALSE);
	} else {
		/*
		 * This shouldn't happen.
		 */
		INSIST(0);
	}

	if (result != DNS_R_WAIT) {
		want_destroy = exit_check(val);
		validator_done(val, result);
	}

	UNLOCK(&val->lock);
	if (want_destroy)
		destroy(val);
}

isc_result_t
dns_validator_create(dns_view_t *view, dns_name_t *name, dns_rdatatype_t type,
		     dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset,
		     dns_message_t *message, unsigned int options,
		     isc_task_t *task, isc_taskaction_t action, void *arg,
		     dns_validator_t **validatorp)
{
	isc_result_t result;
	dns_validator_t *val;
	isc_task_t *tclone;
	dns_validatorevent_t *event;

	REQUIRE(name != NULL);
	REQUIRE(type != 0);
	REQUIRE(rdataset != NULL ||
		(rdataset == NULL && sigrdataset == NULL && message != NULL));
	REQUIRE(options == 0);
	REQUIRE(validatorp != NULL && *validatorp == NULL);

	tclone = NULL;
	result = ISC_R_FAILURE;

	val = isc_mem_get(view->mctx, sizeof(*val));
	if (val == NULL)
		return (ISC_R_NOMEMORY);
	val->view = NULL;
	dns_view_weakattach(view, &val->view);
	event = (dns_validatorevent_t *)
		isc_event_allocate(view->mctx, task,
				   DNS_EVENT_VALIDATORSTART,
				   validator_start, NULL,
				   sizeof(dns_validatorevent_t));
	if (event == NULL) {
		result = ISC_R_NOMEMORY;
		goto cleanup_val;
	}
	isc_task_attach(task, &tclone);
	event->validator = val;
	event->result = ISC_R_FAILURE;
	event->name = name;
	event->type = type;
	event->rdataset = rdataset;
	event->sigrdataset = sigrdataset;
	event->message = message;
	memset(event->proofs, 0, sizeof(event->proofs));
	result = isc_mutex_init(&val->lock);
	if (result != ISC_R_SUCCESS)
		goto cleanup_event;
	val->event = event;
	val->options = options;
	val->attributes = 0;
	val->fetch = NULL;
	val->subvalidator = NULL;
	val->parent = NULL;
	val->keytable = NULL;
	dns_keytable_attach(val->view->secroots, &val->keytable);
	val->keynode = NULL;
	val->key = NULL;
	val->siginfo = NULL;
	val->task = task;
	val->action = action;
	val->arg = arg;
	val->labels = 0;
	val->currentset = NULL;
	val->keyset = NULL;
	val->dsset = NULL;
	val->soaset = NULL;
	val->nsecset = NULL;
	val->soaname = NULL;
	val->seensig = ISC_FALSE;
	dns_rdataset_init(&val->frdataset);
	dns_rdataset_init(&val->fsigrdataset);
	dns_fixedname_init(&val->wild);
	ISC_LINK_INIT(val, link);
	val->magic = VALIDATOR_MAGIC;

	isc_task_send(task, (isc_event_t **) (void *)&event);

	*validatorp = val;

	return (ISC_R_SUCCESS);

 cleanup_event:
	isc_task_detach(&tclone);
	isc_event_free((isc_event_t **)&val->event);

 cleanup_val:
	dns_view_weakdetach(&val->view);
	isc_mem_put(view->mctx, val, sizeof(*val));

	return (result);
}

void
dns_validator_cancel(dns_validator_t *validator) {
	REQUIRE(VALID_VALIDATOR(validator));

	LOCK(&validator->lock);

	validator_log(validator, ISC_LOG_DEBUG(3), "dns_validator_cancel");

	if (validator->event != NULL) {
		if (validator->fetch != NULL)
			dns_resolver_cancelfetch(validator->fetch);

		if (validator->subvalidator != NULL)
			dns_validator_cancel(validator->subvalidator);
	}
	UNLOCK(&validator->lock);
}

static void
destroy(dns_validator_t *val) {
	isc_mem_t *mctx;

	REQUIRE(SHUTDOWN(val));
	REQUIRE(val->event == NULL);
	REQUIRE(val->fetch == NULL);

	if (val->keynode != NULL)
		dns_keytable_detachkeynode(val->keytable, &val->keynode);
	else if (val->key != NULL)
		dst_key_free(&val->key);
	if (val->keytable != NULL)
		dns_keytable_detach(&val->keytable);
	if (val->subvalidator != NULL)
		dns_validator_destroy(&val->subvalidator);
	mctx = val->view->mctx;
	if (val->siginfo != NULL)
		isc_mem_put(mctx, val->siginfo, sizeof(*val->siginfo));
	DESTROYLOCK(&val->lock);
	dns_view_weakdetach(&val->view);
	val->magic = 0;
	isc_mem_put(mctx, val, sizeof(*val));
}

void
dns_validator_destroy(dns_validator_t **validatorp) {
	dns_validator_t *val;
	isc_boolean_t want_destroy = ISC_FALSE;

	REQUIRE(validatorp != NULL);
	val = *validatorp;
	REQUIRE(VALID_VALIDATOR(val));

	LOCK(&val->lock);

	val->attributes |= VALATTR_SHUTDOWN;
	validator_log(val, ISC_LOG_DEBUG(3), "dns_validator_destroy");

	want_destroy = exit_check(val);

	UNLOCK(&val->lock);

	if (want_destroy)
		destroy(val);

	*validatorp = NULL;
}

static void
validator_logv(dns_validator_t *val, isc_logcategory_t *category,
	       isc_logmodule_t *module, int level, const char *fmt, va_list ap)
{
	char msgbuf[2048];

	vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);

	if (val->event != NULL && val->event->name != NULL) {
		char namebuf[DNS_NAME_FORMATSIZE];
		char typebuf[DNS_RDATATYPE_FORMATSIZE];

		dns_name_format(val->event->name, namebuf, sizeof(namebuf));
		dns_rdatatype_format(val->event->type, typebuf,
				     sizeof(typebuf));
		isc_log_write(dns_lctx, category, module, level,
			      "validating %s %s: %s", namebuf, typebuf,
			      msgbuf);
	} else {
		isc_log_write(dns_lctx, category, module, level,
			      "validator @%p: %s", val, msgbuf);
	}
}

static void
validator_log(dns_validator_t *val, int level, const char *fmt, ...) {
	va_list ap;

	if (! isc_log_wouldlog(dns_lctx, level))
		return;

	va_start(ap, fmt);

	validator_logv(val, DNS_LOGCATEGORY_DNSSEC,
		       DNS_LOGMODULE_VALIDATOR, level, fmt, ap);
	va_end(ap);
}

static void
validator_logcreate(dns_validator_t *val,
		    dns_name_t *name, dns_rdatatype_t type,
		    const char *caller, const char *operation)
{
	char namestr[DNS_NAME_FORMATSIZE];
	char typestr[DNS_RDATATYPE_FORMATSIZE];

	dns_name_format(name, namestr, sizeof(namestr));
	dns_rdatatype_format(type, typestr, sizeof(typestr));
	validator_log(val, ISC_LOG_DEBUG(9), "%s: creating %s for %s %s",
		      caller, operation, namestr, typestr);
}
