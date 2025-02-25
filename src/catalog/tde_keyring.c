/*-------------------------------------------------------------------------
 *
 * tde_keyring.c
 *      Deals with the tde keyring configuration catalog
 *      routines.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/catalog/tde_keyring.c
 *
 *-------------------------------------------------------------------------
 */

#include "catalog/tde_keyring.h"
#include "access/skey.h"
#include "access/relscan.h"
#include "utils/builtins.h"
#include "utils/builtins.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "utils/lsyscache.h"
#include "access/heapam.h"
#include "utils/snapmgr.h"
#include "utils/fmgroids.h"

/* Must match the catalog table definition */
#define PG_TDE_KEY_PROVIDER_ID_ATTRNUM 1
#define PG_TDE_KEY_PROVIDER_TYPE_ATTRNUM 2
#define PG_TDE_KEY_PROVIDER_NAME_ATTRNUM 3
#define PG_TDE_KEY_PROVIDER_OPTIONS_ATTRNUM 4

/*
 * These token must be exactly same as defined in
 * pg_tde_add_key_provider_vault_v2 SQL interface
 */
#define VAULTV2_KEYRING_TOKEN_KEY "token"
#define VAULTV2_KEYRING_URL_KEY "url"
#define VAULTV2_KEYRING_MOUNT_PATH_KEY "mountPath"
#define VAULTV2_KEYRING_CA_PATH_KEY "caPath"

/*
 * These token must be exactly same as defined in
 * pg_tde_add_key_provider_file SQL interface
 */
#define FILE_KEYRING_PATH_KEY "path"
#define FILE_KEYRING_TYPE_KEY "type"

static FileKeyring *load_file_keyring_provider_options(Datum keyring_options);
static ProviderType get_keyring_provider_from_typename(char *provider_type);
static GenericKeyring *load_keyring_provider_options(ProviderType provider_type, Datum keyring_options);
static VaultV2Keyring *load_vaultV2_keyring_provider_options(Datum keyring_options);
static void debug_print_kerying(GenericKeyring *keyring);
static GenericKeyring *load_keyring_provider_from_tuple(HeapTuple tuple, TupleDesc tupDesc);

static ProviderType
get_keyring_provider_from_typename(char *provider_type)
{
	if (provider_type == NULL)
		return UNKNOWN_KEY_PROVIDER;

	if (strcmp(FILE_KEYRING_TYPE, provider_type) == 0)
		return FILE_KEY_PROVIDER;
	if (strcmp(VAULTV2_KEYRING_TYPE, provider_type) == 0)
		return VAULT_V2_KEY_PROVIDER;
	return UNKNOWN_KEY_PROVIDER;
}

static GenericKeyring *
load_keyring_provider_from_tuple(HeapTuple tuple, TupleDesc tupDesc)
{
	Datum datum;
	Datum option_datum;
	bool isnull;
	char *keyring_name;
	char *keyring_type;
	int provider_id;
	ProviderType provider_type = UNKNOWN_KEY_PROVIDER;
	GenericKeyring *keyring = NULL;

	datum = heap_getattr(tuple, PG_TDE_KEY_PROVIDER_ID_ATTRNUM, tupDesc, &isnull);
	provider_id = DatumGetInt32(datum);

	datum = heap_getattr(tuple, PG_TDE_KEY_PROVIDER_TYPE_ATTRNUM, tupDesc, &isnull);
	keyring_type = TextDatumGetCString(datum);

	datum = heap_getattr(tuple, PG_TDE_KEY_PROVIDER_NAME_ATTRNUM, tupDesc, &isnull);
	keyring_name = TextDatumGetCString(datum);

	option_datum = heap_getattr(tuple, PG_TDE_KEY_PROVIDER_OPTIONS_ATTRNUM, tupDesc, &isnull);

	provider_type = get_keyring_provider_from_typename(keyring_type);

	keyring = load_keyring_provider_options(provider_type, option_datum);
	if (keyring)
	{
		strncpy(keyring->provider_name, keyring_name, sizeof(keyring->provider_name));
		keyring->key_id = provider_id;
		keyring->type = provider_type;
		debug_print_kerying(keyring);
	}
	return keyring;
}

List *
GetAllKeyringProviders(void)
{
	HeapTuple tuple;
	TupleDesc tupDesc;
	List *keyring_list = NIL;

	Oid pg_tde_schema_oid = LookupNamespaceNoError(PG_TDE_NAMESPACE_NAME);
	Oid kp_table_oid = get_relname_relid(PG_TDE_KEY_PROVIDER_CAT_NAME, pg_tde_schema_oid);
	Relation kp_table_relation = relation_open(kp_table_oid, AccessShareLock);
	TableScanDesc scan;

	tupDesc = kp_table_relation->rd_att;

	scan = heap_beginscan(kp_table_relation, GetLatestSnapshot(), 0, NULL, NULL, 0);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		GenericKeyring *keyring = load_keyring_provider_from_tuple(tuple, tupDesc);
		if (keyring)
		{
			keyring_list = lappend(keyring_list, keyring);
		}
	}
	heap_endscan(scan);
	relation_close(kp_table_relation, AccessShareLock);

	return keyring_list;
}

GenericKeyring *
GetKeyProviderByName(const char *provider_name)
{
	HeapTuple tuple;
	TupleDesc tupDesc;
	TableScanDesc scan;
	ScanKeyData scanKey[1];
	GenericKeyring *keyring = NULL;
	Oid pg_tde_schema_oid = LookupNamespaceNoError(PG_TDE_NAMESPACE_NAME);
	Oid kp_table_oid = get_relname_relid(PG_TDE_KEY_PROVIDER_CAT_NAME, pg_tde_schema_oid);
	Relation kp_table_relation = relation_open(kp_table_oid, AccessShareLock);

	/* Set up a scan key to fetch only required record. */
	ScanKeyInit(scanKey,
				(AttrNumber)PG_TDE_KEY_PROVIDER_NAME_ATTRNUM,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider_name));

	tupDesc = kp_table_relation->rd_att;
	/* Begin the scan with the filter condition */
	scan = heap_beginscan(kp_table_relation, GetLatestSnapshot(), 1, scanKey, NULL, 0);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		keyring = load_keyring_provider_from_tuple(tuple, tupDesc);
		break; /* We expect only one record */
	}
	heap_endscan(scan);
	relation_close(kp_table_relation, AccessShareLock);

	return keyring;
}

GenericKeyring *
GetKeyProviderByID(int provider_id)
{
	HeapTuple tuple;
	TupleDesc tupDesc;
	TableScanDesc scan;
	ScanKeyData scanKey[1];
	GenericKeyring *keyring = NULL;
	Oid pg_tde_schema_oid = LookupNamespaceNoError(PG_TDE_NAMESPACE_NAME);
	Oid kp_table_oid = get_relname_relid(PG_TDE_KEY_PROVIDER_CAT_NAME, pg_tde_schema_oid);
	Relation kp_table_relation = relation_open(kp_table_oid, AccessShareLock);

	/* Set up a scan key to fetch only required record. */
	ScanKeyInit(scanKey,
				(AttrNumber)PG_TDE_KEY_PROVIDER_ID_ATTRNUM,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(provider_id));

	tupDesc = kp_table_relation->rd_att;
	/* Begin the scan with the filter condition */
	scan = heap_beginscan(kp_table_relation, GetLatestSnapshot(), 1, scanKey, NULL, 0);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		keyring = load_keyring_provider_from_tuple(tuple, tupDesc);
		break; /* We only expect 1 record */
	}
	heap_endscan(scan);
	relation_close(kp_table_relation, AccessShareLock);

	return keyring;
}

static GenericKeyring *
load_keyring_provider_options(ProviderType provider_type, Datum keyring_options)
{
	switch (provider_type)
	{
	case FILE_KEY_PROVIDER:
		return (GenericKeyring *)load_file_keyring_provider_options(keyring_options);
		break;
	case VAULT_V2_KEY_PROVIDER:
		return (GenericKeyring *)load_vaultV2_keyring_provider_options(keyring_options);
		break;
	default:
		break;
	}
	return NULL;
}

static FileKeyring *
load_file_keyring_provider_options(Datum keyring_options)
{
	Datum file_path;
	FileKeyring *file_keyring = palloc0(sizeof(FileKeyring));
	file_path = DirectFunctionCall2(json_object_field_text, keyring_options, CStringGetTextDatum(FILE_KEYRING_PATH_KEY));
	file_keyring->keyring.type = FILE_KEY_PROVIDER;
	strncpy(file_keyring->file_name, TextDatumGetCString(file_path), sizeof(file_keyring->file_name));
	return file_keyring;
}

static VaultV2Keyring *
load_vaultV2_keyring_provider_options(Datum keyring_options)
{
	VaultV2Keyring *vaultV2_keyring = palloc0(sizeof(VaultV2Keyring));
	Datum token = DirectFunctionCall2(json_object_field_text, keyring_options, CStringGetTextDatum(VAULTV2_KEYRING_TOKEN_KEY));
	Datum url = DirectFunctionCall2(json_object_field_text, keyring_options, CStringGetTextDatum(VAULTV2_KEYRING_URL_KEY));
	Datum mount_path = DirectFunctionCall2(json_object_field_text, keyring_options, CStringGetTextDatum(VAULTV2_KEYRING_MOUNT_PATH_KEY));
	Datum ca_path = DirectFunctionCall2(json_object_field_text, keyring_options, CStringGetTextDatum(VAULTV2_KEYRING_CA_PATH_KEY));

	vaultV2_keyring->keyring.type = VAULT_V2_KEY_PROVIDER;
	strncpy(vaultV2_keyring->vault_token, TextDatumGetCString(token), sizeof(vaultV2_keyring->vault_token));
	strncpy(vaultV2_keyring->vault_url, TextDatumGetCString(url), sizeof(vaultV2_keyring->vault_url));
	strncpy(vaultV2_keyring->vault_mount_path, TextDatumGetCString(mount_path), sizeof(vaultV2_keyring->vault_mount_path));
	strncpy(vaultV2_keyring->vault_ca_path, TextDatumGetCString(ca_path), sizeof(vaultV2_keyring->vault_ca_path));
	return vaultV2_keyring;
}

static void
debug_print_kerying(GenericKeyring *keyring)
{
	elog(DEBUG2, "Keyring type: %d", keyring->type);
	elog(DEBUG2, "Keyring name: %s", keyring->provider_name);
	elog(DEBUG2, "Keyring id: %d", keyring->key_id);
	switch (keyring->type)
	{
	case FILE_KEY_PROVIDER:
		elog(DEBUG2, "File Keyring Path: %s", ((FileKeyring *)keyring)->file_name);
		break;
	case VAULT_V2_KEY_PROVIDER:
		elog(DEBUG2, "Vault Keyring Token: %s", ((VaultV2Keyring *)keyring)->vault_token);
		elog(DEBUG2, "Vault Keyring URL: %s", ((VaultV2Keyring *)keyring)->vault_url);
		elog(DEBUG2, "Vault Keyring Mount Path: %s", ((VaultV2Keyring *)keyring)->vault_mount_path);
		elog(DEBUG2, "Vault Keyring CA Path: %s", ((VaultV2Keyring *)keyring)->vault_ca_path);
		break;
	case UNKNOWN_KEY_PROVIDER:
		elog(DEBUG2, "Unknown Keyring ");
		break;
	}
}

/* Testing function */
PG_FUNCTION_INFO_V1(pg_tde_get_keyprovider);
Datum pg_tde_get_keyprovider(PG_FUNCTION_ARGS);

Datum pg_tde_get_keyprovider(PG_FUNCTION_ARGS)
{
	GetAllKeyringProviders();
	PG_RETURN_NULL();
}
