// Copyright (c) 2015-2016 Intel Corporation
// Copyright (c) 2023 Nordic Semiconductor ASA
// SPDX-License-Identifier: Apache-2.0
// Exact Zephyr GATT excerpts from 1ee3b93134be23c16ab16b9eb4183bfef63447a6.
// See ../../dependency-overlay/zephyr/LICENSE and upstream gatt.c copyright.
static void db_hash_store(void)
{
#if defined(CONFIG_BT_SETTINGS)
	int err;

	err = bt_settings_store_hash(&db_hash.hash, sizeof(db_hash.hash));
	if (err) {
		LOG_ERR("Failed to save Database Hash (err %d)", err);
	}

	LOG_DBG("Database Hash stored");
#endif	/* CONFIG_BT_SETTINGS */
}
static void do_db_hash(void)
{
	bool new_hash = !atomic_test_bit(gatt_sc.flags, DB_HASH_VALID);

	if (new_hash) {
		db_hash_gen();
	}

#if defined(CONFIG_BT_SETTINGS)
	bool hash_loaded_from_settings =
		atomic_test_bit(gatt_sc.flags, DB_HASH_LOAD);
	bool already_processed =
		atomic_test_bit(gatt_sc.flags, DB_HASH_LOAD_PROC);

	if (!hash_loaded_from_settings) {
		/* we want to generate the hash, but not overwrite the hash
		 * stored in settings, that we haven't yet loaded.
		 */
		return;
	}

	if (already_processed) {
		/* hash has been loaded from settings and we have already
		 * executed the special case below once. we can now safely save
		 * the calculated hash to settings (if it has changed).
		 */
		if (new_hash) {
			set_all_change_unaware();
			db_hash_store();
		}
	} else {
		/* this is only supposed to run once, on bootup, after the hash
		 * has been loaded from settings.
		 */
		atomic_set_bit(gatt_sc.flags, DB_HASH_LOAD_PROC);

		/* Check if hash matches then skip SC update */
		if (!memcmp(db_hash.stored_hash, db_hash.hash,
			    sizeof(db_hash.stored_hash))) {
			LOG_DBG("Database Hash matches");
			k_work_cancel_delayable(&gatt_sc.work);
			atomic_clear_bit(gatt_sc.flags, SC_RANGE_CHANGED);
			return;
		}

		LOG_HEXDUMP_DBG(db_hash.hash, sizeof(db_hash.hash), "New Hash: ");

		/* GATT database has been modified since last boot, likely due
		 * to a firmware update or a dynamic service that was not
		 * re-registered on boot.
		 * Indicate Service Changed to all bonded devices for the full
		 * database range to invalidate client-side cache and force
		 * discovery on reconnect.
		 */
		sc_indicate(0x0001, 0xffff);

		/* Hash did not match, overwrite with current hash.
		 * Also immediately set all peers (in settings) as
		 * change-unaware.
		 */
		set_all_change_unaware();
		db_hash_store();
	}
#endif /* defined(CONFIG_BT_SETTINGS) */
}
static int db_hash_commit(void)
{
	atomic_set_bit(gatt_sc.flags, DB_HASH_LOAD);

	/* Calculate the hash and compare it against the value loaded from
	 * flash. Do it from the current context to avoid any potential race
	 * conditions.
	 */
	do_db_hash();

	return 0;
}
