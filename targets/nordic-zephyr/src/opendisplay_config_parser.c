/*
 * Config load for targets/nordic-zephyr.
 *
 * THE WALK AND THE STORE ARE BOTH SHARED NOW. What was here was a 530-line switch that
 * re-derived, per packet type, the offset arithmetic, the "does this fit" comparison against
 * configLen - 2, the instance caps, and its own CRC-16 and packet-size table. All of that is
 * shared/core/od_config_tlv.c (the walk) and shared/core/od_config.c (the store); what is left
 * is this target's own: reading the blob out of storage, and saying what happened.
 *
 * Two things this file used to do are deliberately NOT carried across:
 *
 *   rescan_security_packet(). It scanned the raw blob for an 0x27 the walk had stepped past, and
 *   existed because an unknown id ends the walk: a blob carrying 0x2C on a build that did not
 *   know 0x2C dropped the security packet behind it. The shared size table knows every canonical
 *   id, so the case it recovered from cannot arise for a canonical packet; keeping the scan would
 *   carry a workaround into the one path a client can write pre-auth, and would leave the next
 *   missing id silently survivable. See shared/core/od_config.h.
 *
 *   The per-packet "known-unparsed, skipping N bytes" arm. Every id in the table is dispatched
 *   and stored now, so there is no such thing as known-but-unparsed here.
 */

#include "opendisplay_config_parser.h"
#include "od_log.h"
#include "opendisplay_config_storage.h"

/* The config parsed last, for the two security accessors below. A pointer rather than a copy:
 * security lives inside the aggregate so that nothing can read an encryption_enabled the
 * zero-key rule has not been applied to, and a second copy here would be exactly that hole. */
static const struct od_config *s_parsed;

/* Returned before any parse has run. Non-NULL so the callers that check for NULL keep their
 * meaning: no config means no encryption, not "unknown". */
static const struct SecurityConfig s_no_security;

/* The BT RX thread needs only this derived fact. Publishing one byte after a parse finishes
 * keeps it away from the loop thread's mutable config aggregate. */
static uint8_t s_security_enabled_snapshot;

static void publish_security_enabled(const struct od_config *cfg)
{
	/* The 2.x rule (enabled and a non-zero key), for the RX-thread debug predicate only; whether a
	 * session is required is od_sec2's state, which lives on the main thread. */
	bool enabled = false;
	unsigned i;

	if (cfg != NULL && cfg->security.encryption_enabled != 0u) {
		for (i = 0; i < sizeof(cfg->security.encryption_key); ++i) {
			enabled = enabled || cfg->security.encryption_key[i] != 0u;
		}
	}

	__atomic_store_n(&s_security_enabled_snapshot, enabled ? 1u : 0u, __ATOMIC_RELEASE);
}

const struct SecurityConfig *od_get_parsed_security(void)
{
	return (s_parsed != NULL) ? &s_parsed->security : &s_no_security;
}

bool od_security_enabled_snapshot(void)
{
	return __atomic_load_n(&s_security_enabled_snapshot, __ATOMIC_ACQUIRE) != 0u;
}

bool parseConfigBytes(uint8_t *configData, uint32_t configLen, struct od_config *globalConfig)
{
	enum od_config_tlv_result walk;

	/* Resets, walks, stores, normalises, and computes the advisory CRC. globalConfig->loaded is
	 * set only on a clean walk; a blob that truncates half-way keeps the packets that preceded
	 * the truncation, so `loaded` is what callers must read, not the counts. */
	walk = od_config_parse(globalConfig, od_span_make(configData, configLen), NULL);
	s_parsed = globalConfig;
	publish_security_enabled(globalConfig);

	if (walk != OD_CFG_TLV_OK) {
		return false;
	}

	return true;
}

bool loadGlobalConfig(struct od_config *globalConfig)
{
	static uint8_t configData[OD_CONFIG_MAX_SIZE];
	uint32_t configLen = OD_CONFIG_MAX_SIZE;

	if (globalConfig == NULL) {
		od_log_error("Config load rejected: invalid destination");
		publish_security_enabled(NULL);
		return false;
	}

	od_config_reset(globalConfig);
	s_parsed = globalConfig;

	if (!initConfigStorage()) {
		publish_security_enabled(globalConfig);
		return false;
	}

	if (!loadConfig(configData, &configLen)) {
		publish_security_enabled(globalConfig);
		return false;
	}

	return parseConfigBytes(configData, configLen, globalConfig);
}
