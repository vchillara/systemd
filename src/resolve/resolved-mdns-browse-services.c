/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "af-list.h"
#include "event-util.h"
#include "random-util.h"
#include "resolved-dns-cache.h"
#include "resolved-mdns-browse-services.h"
#include "resolved-varlink.h"

/* RFC 6762 section 5.2 - A random variation of 2% of the record TTL should
 * be added to maintenance queries. */
#define MDNS_MAINTENANCE_JITTER (random_u64_range(100) * 2)

/* RFC 6762 section 5.2 - The querier should plan to issue a query at 80% of
 * the record lifetime, and then if no answer is received, at 85%, 90%, and 95%.
 * Adding 1 to TTL to compensate for clamped TTL. */
static usec_t mdns_maintenance_next(usec_t usec, usec_t ttl, int base_perc) {
        return usec_add(usec, (base_perc) * (1 + ttl) * USEC_PER_SEC / 100);
}

static usec_t mdns_maintenance_next_with_jitter(usec_t usec, usec_t ttl, int base_perc) {
        usec_t jitter = MDNS_MAINTENANCE_JITTER * (1 + ttl) * USEC_PER_SEC / 10000;
        return usec_add(mdns_maintenance_next(usec, ttl, base_perc), jitter);
}

#define MDNS_80_PERCENT 80
#define MDNS_5_PERCENT 5

typedef struct mDnsGoodbyeParams mDnsGoodbyeParams;

struct mDnsGoodbyeParams {
        DnsServiceBrowser *ss;
        int owner_family;
};

mDnsGoodbyeParams *mdns_goodbye_params_free(mDnsGoodbyeParams *p);

DEFINE_TRIVIAL_CLEANUP_FUNC(mDnsGoodbyeParams*, mdns_goodbye_params_free);

static void mdns_find_service_from_query(DnsService **service, DnsServiceBrowser *ss, DnsQuery *q) {
        assert(ss);

        /* Find the service that owns the query. */
        LIST_FOREACH(dns_services, s, ss->dns_services) {
                if (s->query == q) {
                        *service = s;
                        return;
                }
        }
        *service = NULL;
}

static void mdns_maintenance_query_complete(DnsQuery *q) {
        _cleanup_(dns_service_browser_unrefp) DnsServiceBrowser *ss = NULL;
        _cleanup_(dns_query_freep) DnsQuery *query = q;
        DnsService *service = NULL;
        int  r;

        assert(query);
        assert(query->manager);

        ss = dns_service_browser_ref(hashmap_get(query->manager->dns_service_browsers, query->varlink_request));
        if (!ss)
                return;

        if (query->state != DNS_TRANSACTION_SUCCESS) {
                r = 0;
                goto finish;
        }

        r = dns_answer_match_key(query->answer, ss->key, NULL);
        if (r <= 0)
                goto finish;

        r = mdns_browser_lookup_cache(ss, query->answer_family);

finish:
        if (r < 0)
                log_error_errno(r, "mDNS maintenance query complete failed: %m");

        mdns_find_service_from_query(&service, ss, query);
        if (service)
                service->query = NULL;
}

static int mdns_maintenance_query(sd_event_source *s, uint64_t usec, void *userdata) {
        DnsService *service = NULL;
        _cleanup_(dns_query_freep) DnsQuery *q = NULL;
        int r;

        assert(userdata);

        service = userdata;

        if (service->rr_ttl_state++ == MDNS_TTL_100_PERCENT)
                return mdns_browser_lookup_cache(service->ss, service->family);

        r = dns_query_new(service->ss->m, &q, service->ss->question_utf8, service->ss->question_idna, NULL, service->ss->ifindex, service->ss->flags);
        if (r < 0)
                goto finish;

        q->complete = mdns_maintenance_query_complete;
        q->varlink_request = varlink_ref(service->ss->link);

        r = dns_query_go(q);
        if (r < 0)
                goto finish;

        usec_t next_time = mdns_maintenance_next(usec, service->rr->ttl, MDNS_5_PERCENT);

        r = event_reset_time(
                service->ss->m->event, &service->schedule_event,
                CLOCK_BOOTTIME, next_time, 0, mdns_maintenance_query,
                service, 0, "mdns-next-query-schedule", true);
        if (r < 0)
                goto finish;

        service->query = TAKE_PTR(q);

        return 0;

finish:
        return log_error_errno(r, "Failed mdns maintenance query: %m");
}

int dns_add_new_service(DnsServiceBrowser *ss, DnsResourceRecord *rr, int owner_family) {
        _cleanup_(dns_service_unrefp) DnsService *s = NULL;
        int r;

        assert(ss);
        assert(rr);

        s = new(DnsService, 1);
        if (!s)
                return log_oom();

        usec_t usec = now(CLOCK_BOOTTIME);

        *s = (DnsService) {
                .n_ref = 1,
                .ss = dns_service_browser_ref(ss),
                .rr = dns_resource_record_copy(rr),
                .family = owner_family,
                .until = usec_add(usec, (rr->ttl + 1) * USEC_PER_SEC),
                .rr_ttl_state = MDNS_TTL_80_PERCENT,
        };

        LIST_PREPEND(dns_services, ss->dns_services, s);

        usec_t next_time = mdns_maintenance_next_with_jitter(usec, rr->ttl, MDNS_80_PERCENT);
        /* Schedule the first cache maintenance query at 80% of the record's TTL.
         * RFC 6762 section 5.2. */
        r = sd_event_add_time(
                        ss->m->event,
                        &s->schedule_event,
                        CLOCK_BOOTTIME,
                        next_time,
                        0,
                        mdns_maintenance_query,
                        s);
        if (r < 0)
                return r;

        TAKE_PTR(s);
        return 0;
}

void dns_remove_service(DnsServiceBrowser *ss, DnsService *service) {
        assert(ss);
        assert(service);

        LIST_REMOVE(dns_services, ss->dns_services, service);
        dns_service_free(service);
}

DnsService *dns_service_free(DnsService *service) {
        if (!service)
                return NULL;

        sd_event_source_disable_unref(service->schedule_event);

        if (service->query && DNS_TRANSACTION_IS_LIVE(service->query->state))
                        dns_query_complete(service->query, DNS_TRANSACTION_ABORTED);

        dns_service_browser_unref(service->ss);

        dns_resource_record_unref(service->rr);

        return mfree(service);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(DnsService, dns_service, dns_service_free);

int mdns_service_update(DnsService *service, DnsResourceRecord *rr, usec_t t) {
        service->until = t + rr->ttl*USEC_PER_SEC;
        service->rr_ttl_state = MDNS_TTL_80_PERCENT;

        /* Update the 80% TTL maintenance event based on new record received from the network.
         * RFC 6762 section 5.2  */
        usec_t next_time = mdns_maintenance_next_with_jitter(t, rr->ttl, MDNS_80_PERCENT);
        if (service->schedule_event)
                return sd_event_source_set_time(service->schedule_event, next_time);

        return 0;
}

bool dns_service_contains(DnsService *services, DnsResourceRecord *rr, int owner_family) {
        usec_t t = now(CLOCK_BOOTTIME);

        LIST_FOREACH(dns_services, service, services)
                if (dns_resource_record_equal(rr, service->rr) > 0 && service->family == owner_family) {
                        if (usec_add(t, rr->ttl * USEC_PER_SEC) > service->until)
                                mdns_service_update(service, rr, t);

                        return true;
                }

        return false;
}

void dns_browse_services_purge(Manager *m, int family) {
        int r = 0;

        /* Called after caches are flused.
         * Clear local service records and notify varlink client. */
        if (!(m && m->dns_service_browsers))
                return;

        DnsServiceBrowser *ss;
        HASHMAP_FOREACH(ss, m->dns_service_browsers) {
                r = sd_event_source_set_enabled(ss->schedule_event, SD_EVENT_OFF);
                if (r < 0)
                        goto finish;

                if (family == AF_UNSPEC) {
                        r = mdns_browser_lookup_cache(ss, AF_INET);
                        if (r < 0)
                                goto finish;
                        r = mdns_browser_lookup_cache(ss, AF_INET6);
                        if (r < 0)
                                goto finish;
                        return;
                }

                r = mdns_browser_lookup_cache(ss, family);
                if (r < 0)
                        goto finish;
        }

finish:
        if (r < 0)
                log_error_errno(r, "mdns browse services purge failed: %m");
        return;
}

int mdns_manage_services_answer(DnsServiceBrowser *ss, DnsAnswer *answer, int owner_family) {
        DnsResourceRecord *i;
        _cleanup_(json_variant_unrefp) JsonVariant *array = NULL;
        int r;

        assert(ss);

        /* Check for new service added */
        DNS_ANSWER_FOREACH(i, answer) {
                _cleanup_free_ char *name = NULL, *type = NULL, *domain = NULL;
                _cleanup_(json_variant_unrefp) JsonVariant *entry = NULL;

                if (dns_service_contains(ss->dns_services, i, owner_family))
                        continue;

                r = dns_service_split(i->ptr.name, &name, &type, &domain);
                if (r < 0)
                        goto finish;

                r = dns_add_new_service(ss, i, owner_family);
                if (r < 0)
                        goto finish;

                log_debug("Add into the list  %s, %s, %s, %s, %d",
                          strna(name),
                          strna(type),
                          strna(domain),
                          strna(af_to_ipv4_ipv6(owner_family)),
                          ss->ifindex);

                r = json_build(&entry,
                                JSON_BUILD_OBJECT(
                                                JSON_BUILD_PAIR("add_flag", JSON_BUILD_BOOLEAN(true)),
                                                JSON_BUILD_PAIR("family", JSON_BUILD_INTEGER(owner_family)),
                                                JSON_BUILD_PAIR("name", JSON_BUILD_STRING(name)),
                                                JSON_BUILD_PAIR("type", JSON_BUILD_STRING(type)),
                                                JSON_BUILD_PAIR("domain", JSON_BUILD_STRING(domain)),
                                                JSON_BUILD_PAIR("interface", JSON_BUILD_INTEGER(ss->ifindex))));
                if (r < 0)
                        goto finish;

                r = json_variant_append_array(&array, entry);
                if (r < 0)
                        goto finish;
        }

        /* Check for services removed */
        LIST_FOREACH(dns_services, service, ss->dns_services) {
                _cleanup_free_ char *name = NULL, *type = NULL, *domain = NULL;
                _cleanup_(json_variant_unrefp) JsonVariant *entry = NULL;

                if (service->family != owner_family)
                        continue;

                if (dns_answer_contains(answer, service->rr))
                        continue;

                r = dns_service_split(service->rr->ptr.name, &name, &type, &domain);
                if (r < 0)
                        goto finish;

                dns_remove_service(ss, service);

                log_debug("Remove from the list  %s, %s, %s, %s, %d",
                          strna(name),
                          strna(type),
                          strna(domain),
                          strna(af_to_ipv4_ipv6(owner_family)),
                          ss->ifindex);

                r = json_build(&entry,
                                JSON_BUILD_OBJECT(
                                                JSON_BUILD_PAIR("add_flag", JSON_BUILD_BOOLEAN(false)),
                                                JSON_BUILD_PAIR("family", JSON_BUILD_INTEGER(owner_family)),
                                                JSON_BUILD_PAIR("name", JSON_BUILD_STRING(name)),
                                                JSON_BUILD_PAIR("type", JSON_BUILD_STRING(type)),
                                                JSON_BUILD_PAIR("domain", JSON_BUILD_STRING(domain)),
                                                JSON_BUILD_PAIR("interface", JSON_BUILD_INTEGER(ss->ifindex))));
                if (r < 0)
                        goto finish;

                r = json_variant_append_array(&array, entry);
                if (r < 0)
                        goto finish;
        }

        if (!json_variant_is_blank_array(array)) {
                _cleanup_(json_variant_unrefp) JsonVariant *vm = NULL;

                r = json_build(&vm,
                                JSON_BUILD_OBJECT(
                                                JSON_BUILD_PAIR("browser_service_data", JSON_BUILD_VARIANT(array))));
                if (r < 0)
                        goto finish;

                r = varlink_notify(ss->link, vm);
                if (r < 0)
                        goto finish;
        }

        return 0;

finish:
        log_error_errno(r, "Failed to process received services: %m");
        return varlink_error_errno(ss->link, r);
}

int mdns_browser_lookup_cache(DnsServiceBrowser *ss, int owner_family) {
        _cleanup_(dns_answer_unrefp) DnsAnswer *lookup_ret_answer = NULL;
        DnsScope *scope;
        int r;

        assert(ss);
        assert(ss->m);

        scope = manager_find_scope_from_protocol(ss->m, ss->ifindex, DNS_PROTOCOL_MDNS, owner_family);
        if (!scope)
                return 0;

        dns_cache_prune(&scope->cache);

        r = dns_cache_lookup(
                        &scope->cache,
                        ss->key,
                        ss->flags | SD_RESOLVED_CLAMP_TTL,
                        NULL,
                        &lookup_ret_answer,
                        NULL,
                        NULL,
                        NULL);
        if (r < 0)
                return r;

        return mdns_manage_services_answer(ss, lookup_ret_answer, owner_family);
}

static int goodbye_callback(sd_event_source *s, uint64_t usec, void *userdata) {
        _cleanup_(mdns_goodbye_params_freep) mDnsGoodbyeParams *cb_params = userdata;
        int r;

        if (!cb_params->ss)
                return 0;

        r = mdns_browser_lookup_cache(cb_params->ss, cb_params->owner_family);
        if (r < 0)
                return log_error_errno(r, "Failed to lookup cache: %m");

        return 0;
}

int mdns_notify_browsers_unsolicited_updates(Manager *m, DnsAnswer *answer, int owner_family, bool has_goodbye) {
        _cleanup_(dns_service_browser_unrefp) DnsServiceBrowser *ss = NULL;
        int r;

        assert(answer);
        assert(m);

        if (!m->dns_service_browsers)
                return 0;

        HASHMAP_FOREACH(ss, m->dns_service_browsers) {
                r = dns_answer_match_key(answer, ss->key, NULL);
                if (r < 0)
                        goto finish;
                else if (r == 0)
                        continue;

                if (has_goodbye) {
                        _cleanup_(mdns_goodbye_params_freep) mDnsGoodbyeParams *cb_params = NULL;

                        cb_params = new(mDnsGoodbyeParams, 1);
                        if (!cb_params)
                                return log_oom();

                        *cb_params = (mDnsGoodbyeParams) {
                                .ss = dns_service_browser_ref(ss),
                                .owner_family = owner_family,
                        };

                        r = sd_event_add_time_relative(
                                m->event,
                                NULL,
                                CLOCK_BOOTTIME,
                                USEC_PER_SEC,
                                0,
                                goodbye_callback,
                                cb_params
                        );
                        if (r < 0)
                                goto finish;

                        TAKE_PTR(cb_params);
                        continue;
                }

                r = mdns_browser_lookup_cache(ss, owner_family);
                if (r < 0)
                        goto finish;
        }

        return 0;

finish:
        return log_error_errno(r, "Failed to notify mDNS service subscribers, %m");
}

static void mdns_browse_service_query_complete(DnsQuery *q) {
        _cleanup_(dns_service_browser_unrefp) DnsServiceBrowser *ss = NULL;
        _cleanup_(dns_query_freep) DnsQuery *query = q;
        int r;

        assert(query);
        assert(query->manager);

        if (query->state != DNS_TRANSACTION_SUCCESS)
                return;

        ss = dns_service_browser_ref(hashmap_get(query->manager->dns_service_browsers, query->varlink_request));
        if (!ss)
                return;

        r = dns_answer_match_key(query->answer, ss->key, NULL);
        if (r < 0)
                goto finish;
        else if (r == 0)
                return;

        r = mdns_browser_lookup_cache(ss, query->answer_family);
        if (r < 0)
                goto finish;

        /* When the query is answered from cache, we only get answers for one answer_family
         * i.e. either ipv4 or ipv6.
         * We need to perform another cache lookup for the other answer_family */
        if (query->answer_query_flags == SD_RESOLVED_FROM_CACHE) {
                r = mdns_browser_lookup_cache(ss, query->answer_family == AF_INET? AF_INET6 : AF_INET);
                if (r < 0)
                        goto finish;
        }

        return;
finish:
        log_error_errno(r, "mDNS browse query complete failed, %m");
}

static int mdns_next_query_schedule(sd_event_source *s, uint64_t usec, void *userdata) {
        _cleanup_(dns_service_browser_unrefp) DnsServiceBrowser *ss = NULL;
        _cleanup_(dns_query_freep) DnsQuery *q = NULL;
        int r;

        assert(userdata);

        ss = dns_service_browser_ref(userdata);

        r = dns_query_new(ss->m, &q, ss->question_utf8, ss->question_idna, NULL, ss->ifindex, ss->flags);
        if (r < 0)
                goto finish;

        q->complete = mdns_browse_service_query_complete;
        q->varlink_request = varlink_ref(ss->link);
        varlink_set_userdata(ss->link, q);

        r = dns_query_go(q);
        if (r < 0)
                goto finish;

        /* RFC6762 5.2
         * The intervals between successive queries MUST increase by at least a factor of two.
         * When the interval between queries reaches or exceeds 60 minutes,perform
         * subsequent queries at a steady-state rate of one query per hour */
        if (ss->delay == 0) {
                ss->delay++;
                /* First query is sent wihtout SD_RESOLVED_NO_CACHE to fetch answers already in cache.
                 * Set SD_RESOLVED_NO_CACHE to make all subsequent queries go to the network. */
                ss->flags |= SD_RESOLVED_NO_CACHE;
        }
        else
                ss->delay = ss->delay < 2048 ? ss->delay * 2 : 3600;

        r = event_reset_time_relative(
                ss->m->event, &ss->schedule_event,
                CLOCK_BOOTTIME, (ss->delay * USEC_PER_SEC),
                0, mdns_next_query_schedule,
                ss, 0, "mdns-next-query-schedule", true);
        if (r < 0)
                goto finish;

        TAKE_PTR(q);

        return 0;

finish:
        return log_error_errno(r, "Failed to schedule mDNS query, %m");
}

void dns_service_browser_reset(Manager *m) {
        int r;

        if (!(m && m->dns_service_browsers))
                return;

        DnsServiceBrowser *ss;

        HASHMAP_FOREACH(ss, m->dns_service_browsers) {
                ss->delay = 0;

                r = event_reset_time_relative(
                        ss->m->event, &ss->schedule_event,
                        CLOCK_BOOTTIME, (ss->delay * USEC_PER_SEC),
                        0, mdns_next_query_schedule,
                        ss, 0, "mdns-next-query-schedule", true);
                if (r < 0)
                        log_error_errno(r, "Failed to reset mdns service subscriber, %m");
        }

        return;
}

int dns_subscribe_browse_service(
                Manager *m,
                Varlink *link,
                const char *domain,
                const char *name,
                const char *type,
                int ifindex,
                uint64_t flags) {

        _cleanup_(dns_service_browser_unrefp) DnsServiceBrowser *ss = NULL;
        _cleanup_(dns_question_unrefp) DnsQuestion *question_idna = NULL, *question_utf8 = NULL;
        int r;

        assert(m);
        assert(link);

        if (ifindex <= 0)
                return varlink_error_invalid_parameter(link, JSON_VARIANT_STRING_CONST("ifindex"));

        if (isempty(name))
                name = NULL;
        else if (!dns_service_name_is_valid(name))
                return varlink_error_invalid_parameter(link, JSON_VARIANT_STRING_CONST("name"));

        if (isempty(type))
                type = NULL;
        else if (!dnssd_srv_type_is_valid(type))
                return varlink_error_invalid_parameter(link, JSON_VARIANT_STRING_CONST("type"));

        r = dns_name_is_valid(domain);
        if (r < 0)
                return r;
        if (r == 0)
                return varlink_error_invalid_parameter(link, JSON_VARIANT_STRING_CONST("domain"));

        r = dns_question_new_service_type(&question_utf8, name, type, domain, false, DNS_TYPE_PTR);
        if (r < 0)
                return r;

        r = dns_question_new_service_type(&question_idna, name, type, domain, true, DNS_TYPE_PTR);
        if (r < 0)
                return r;

        ss = new(DnsServiceBrowser, 1);
        if (!ss)
                return log_oom();

        *ss = (DnsServiceBrowser) {
                .n_ref = 1,
                .m = m,
                .link = varlink_ref(link),
                .question_utf8 = dns_question_ref(question_utf8),
                .question_idna = dns_question_ref(question_idna),
                .key = dns_question_first_key(question_utf8),
                .ifindex = ifindex,
                .flags = flags,
        };

        switch (flags) {
        case SD_RESOLVED_MDNS:
                r = sd_event_add_time(m->event,
                                &ss->schedule_event,
                                CLOCK_BOOTTIME,
                                usec_add(now(CLOCK_BOOTTIME), (ss->delay * USEC_PER_SEC)),
                                0,
                                mdns_next_query_schedule,
                                ss);
                if (r < 0)
                        return r;
                break;
        default:
                return -EINVAL;
        }

        r = hashmap_ensure_put(&m->dns_service_browsers, NULL, link, TAKE_PTR(ss));

        return r;
}

DnsServiceBrowser *dns_service_browser_free(DnsServiceBrowser *ss) {
        DnsQuery *q;

        if (!ss)
                return NULL;

        LIST_FOREACH(dns_services, service, ss->dns_services)
                dns_remove_service(ss, service);

        sd_event_source_disable_unref(ss->schedule_event);

        q = varlink_get_userdata(ss->link);
        if (q && DNS_TRANSACTION_IS_LIVE(q->state))
                dns_query_complete(q, DNS_TRANSACTION_ABORTED);

        dns_question_unref(ss->question_idna);
        dns_question_unref(ss->question_utf8);

        varlink_unref(ss->link);

        return mfree(ss);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(DnsServiceBrowser, dns_service_browser, dns_service_browser_free);

mDnsGoodbyeParams* mdns_goodbye_params_free(mDnsGoodbyeParams *p) {
        if (!p)
                return NULL;

        dns_service_browser_unref(p->ss);
        return mfree(p);
}