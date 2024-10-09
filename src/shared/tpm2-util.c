/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "constants.h"
#include "cryptsetup-util.h"
#include "dirent-util.h"
#include "dlfcn-util.h"
#include "efi-api.h"
#include "extract-word.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-table.h"
#include "fs-util.h"
#include "hexdecoct.h"
#include "hmac.h"
#include "initrd-util.h"
#include "lock-util.h"
#include "log.h"
#include "logarithm.h"
#include "memory-util.h"
#include "nulstr-util.h"
#include "openssl-util.h"
#include "parse-util.h"
#include "random-util.h"
#include "sha256.h"
#include "stat-util.h"
#include "string-table.h"
#include "time-util.h"
#include "tpm2-util.h"
#include "virt.h"

#if HAVE_TPM2
static void *libtss2_esys_dl = NULL;
static void *libtss2_rc_dl = NULL;
static void *libtss2_mu_dl = NULL;

static TSS2_RC (*sym_Esys_Create)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_PUBLIC *inPublic, const TPM2B_DATA *outsideInfo, const TPML_PCR_SELECTION *creationPCR, TPM2B_PRIVATE **outPrivate, TPM2B_PUBLIC **outPublic, TPM2B_CREATION_DATA **creationData, TPM2B_DIGEST **creationHash, TPMT_TK_CREATION **creationTicket) = NULL;
static TSS2_RC (*sym_Esys_CreateLoaded)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_TEMPLATE *inPublic, ESYS_TR *objectHandle, TPM2B_PRIVATE **outPrivate, TPM2B_PUBLIC **outPublic) = NULL;
static TSS2_RC (*sym_Esys_CreatePrimary)(ESYS_CONTEXT *esysContext, ESYS_TR primaryHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_PUBLIC *inPublic, const TPM2B_DATA *outsideInfo, const TPML_PCR_SELECTION *creationPCR, ESYS_TR *objectHandle, TPM2B_PUBLIC **outPublic, TPM2B_CREATION_DATA **creationData, TPM2B_DIGEST **creationHash, TPMT_TK_CREATION **creationTicket) = NULL;
static TSS2_RC (*sym_Esys_EvictControl)(ESYS_CONTEXT *esysContext, ESYS_TR auth, ESYS_TR objectHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPMI_DH_PERSISTENT persistentHandle, ESYS_TR *newObjectHandle) = NULL;
static void (*sym_Esys_Finalize)(ESYS_CONTEXT **context) = NULL;
static TSS2_RC (*sym_Esys_FlushContext)(ESYS_CONTEXT *esysContext, ESYS_TR flushHandle) = NULL;
static void (*sym_Esys_Free)(void *ptr) = NULL;
static TSS2_RC (*sym_Esys_GetCapability)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2_CAP capability, UINT32 property, UINT32 propertyCount, TPMI_YES_NO *moreData, TPMS_CAPABILITY_DATA **capabilityData) = NULL;
static TSS2_RC (*sym_Esys_GetRandom)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, UINT16 bytesRequested, TPM2B_DIGEST **randomBytes) = NULL;
static TSS2_RC (*sym_Esys_Initialize)(ESYS_CONTEXT **esys_context,  TSS2_TCTI_CONTEXT *tcti, TSS2_ABI_VERSION *abiVersion) = NULL;
static TSS2_RC (*sym_Esys_Load)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_PRIVATE *inPrivate, const TPM2B_PUBLIC *inPublic, ESYS_TR *objectHandle) = NULL;
static TSS2_RC (*sym_Esys_LoadExternal)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE *inPrivate, const TPM2B_PUBLIC *inPublic, ESYS_TR hierarchy, ESYS_TR *objectHandle) = NULL;
static TSS2_RC (*sym_Esys_PCR_Extend)(ESYS_CONTEXT *esysContext, ESYS_TR pcrHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPML_DIGEST_VALUES *digests) = NULL;
static TSS2_RC (*sym_Esys_PCR_Read)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1,ESYS_TR shandle2, ESYS_TR shandle3, const TPML_PCR_SELECTION *pcrSelectionIn, UINT32 *pcrUpdateCounter, TPML_PCR_SELECTION **pcrSelectionOut, TPML_DIGEST **pcrValues) = NULL;
static TSS2_RC (*sym_Esys_PolicyAuthorize)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *approvedPolicy, const TPM2B_NONCE *policyRef, const TPM2B_NAME *keySign, const TPMT_TK_VERIFIED *checkTicket) = NULL;
static TSS2_RC (*sym_Esys_PolicyAuthValue)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3) = NULL;
static TSS2_RC (*sym_Esys_PolicyGetDigest)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_DIGEST **policyDigest) = NULL;
static TSS2_RC (*sym_Esys_PolicyPCR)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *pcrDigest, const TPML_PCR_SELECTION *pcrs) = NULL;
static TSS2_RC (*sym_Esys_ReadPublic)(ESYS_CONTEXT *esysContext, ESYS_TR objectHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_PUBLIC **outPublic, TPM2B_NAME **name, TPM2B_NAME **qualifiedName) = NULL;
static TSS2_RC (*sym_Esys_StartAuthSession)(ESYS_CONTEXT *esysContext, ESYS_TR tpmKey, ESYS_TR bind, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_NONCE *nonceCaller, TPM2_SE sessionType, const TPMT_SYM_DEF *symmetric, TPMI_ALG_HASH authHash, ESYS_TR *sessionHandle) = NULL;
static TSS2_RC (*sym_Esys_Startup)(ESYS_CONTEXT *esysContext, TPM2_SU startupType) = NULL;
static TSS2_RC (*sym_Esys_TestParms)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPMT_PUBLIC_PARMS *parameters) = NULL;
static TSS2_RC (*sym_Esys_TR_Close)(ESYS_CONTEXT *esys_context, ESYS_TR *rsrc_handle) = NULL;
static TSS2_RC (*sym_Esys_TR_Deserialize)(ESYS_CONTEXT *esys_context, uint8_t const *buffer, size_t buffer_size, ESYS_TR *esys_handle) = NULL;
static TSS2_RC (*sym_Esys_TR_FromTPMPublic)(ESYS_CONTEXT *esysContext, TPM2_HANDLE tpm_handle, ESYS_TR optionalSession1, ESYS_TR optionalSession2, ESYS_TR optionalSession3, ESYS_TR *object) = NULL;
static TSS2_RC (*sym_Esys_TR_GetName)(ESYS_CONTEXT *esysContext, ESYS_TR handle, TPM2B_NAME **name) = NULL;
static TSS2_RC (*sym_Esys_TR_Serialize)(ESYS_CONTEXT *esys_context, ESYS_TR object, uint8_t **buffer, size_t *buffer_size) = NULL;
static TSS2_RC (*sym_Esys_TR_SetAuth)(ESYS_CONTEXT *esysContext, ESYS_TR handle, TPM2B_AUTH const *authValue) = NULL;
static TSS2_RC (*sym_Esys_TRSess_GetAttributes)(ESYS_CONTEXT *esysContext, ESYS_TR session, TPMA_SESSION *flags) = NULL;
static TSS2_RC (*sym_Esys_TRSess_SetAttributes)(ESYS_CONTEXT *esysContext, ESYS_TR session, TPMA_SESSION flags, TPMA_SESSION mask) = NULL;
static TSS2_RC (*sym_Esys_Unseal)(ESYS_CONTEXT *esysContext, ESYS_TR itemHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_SENSITIVE_DATA **outData) = NULL;
static TSS2_RC (*sym_Esys_VerifySignature)(ESYS_CONTEXT *esysContext, ESYS_TR keyHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *digest, const TPMT_SIGNATURE *signature, TPMT_TK_VERIFIED **validation) = NULL;

static TSS2_RC (*sym_Tss2_MU_TPM2_CC_Marshal)(TPM2_CC src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_PRIVATE_Marshal)(TPM2B_PRIVATE const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_PRIVATE_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_PRIVATE  *dest) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_PUBLIC_Marshal)(TPM2B_PUBLIC const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_PUBLIC_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_PUBLIC *dest) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPML_PCR_SELECTION_Marshal)(TPML_PCR_SELECTION const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPMT_HA_Marshal)(TPMT_HA const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPMT_PUBLIC_Marshal)(TPMT_PUBLIC const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;

static const char* (*sym_Tss2_RC_Decode)(TSS2_RC rc) = NULL;

int dlopen_tpm2(void) {
        int r;

        r = dlopen_many_sym_or_warn(
                        &libtss2_esys_dl, "libtss2-esys.so.0", LOG_DEBUG,
                        DLSYM_ARG(Esys_Create),
                        DLSYM_ARG(Esys_CreateLoaded),
                        DLSYM_ARG(Esys_CreatePrimary),
                        DLSYM_ARG(Esys_EvictControl),
                        DLSYM_ARG(Esys_Finalize),
                        DLSYM_ARG(Esys_FlushContext),
                        DLSYM_ARG(Esys_Free),
                        DLSYM_ARG(Esys_GetCapability),
                        DLSYM_ARG(Esys_GetRandom),
                        DLSYM_ARG(Esys_Initialize),
                        DLSYM_ARG(Esys_Load),
                        DLSYM_ARG(Esys_LoadExternal),
                        DLSYM_ARG(Esys_PCR_Extend),
                        DLSYM_ARG(Esys_PCR_Read),
                        DLSYM_ARG(Esys_PolicyAuthorize),
                        DLSYM_ARG(Esys_PolicyAuthValue),
                        DLSYM_ARG(Esys_PolicyGetDigest),
                        DLSYM_ARG(Esys_PolicyPCR),
                        DLSYM_ARG(Esys_ReadPublic),
                        DLSYM_ARG(Esys_StartAuthSession),
                        DLSYM_ARG(Esys_Startup),
                        DLSYM_ARG(Esys_TestParms),
                        DLSYM_ARG(Esys_TR_Close),
                        DLSYM_ARG(Esys_TR_Deserialize),
                        DLSYM_ARG(Esys_TR_FromTPMPublic),
                        DLSYM_ARG(Esys_TR_GetName),
                        DLSYM_ARG(Esys_TR_Serialize),
                        DLSYM_ARG(Esys_TR_SetAuth),
                        DLSYM_ARG(Esys_TRSess_GetAttributes),
                        DLSYM_ARG(Esys_TRSess_SetAttributes),
                        DLSYM_ARG(Esys_Unseal),
                        DLSYM_ARG(Esys_VerifySignature));
        if (r < 0)
                return r;

        r = dlopen_many_sym_or_warn(
                        &libtss2_rc_dl, "libtss2-rc.so.0", LOG_DEBUG,
                        DLSYM_ARG(Tss2_RC_Decode));
        if (r < 0)
                return r;

        return dlopen_many_sym_or_warn(
                        &libtss2_mu_dl, "libtss2-mu.so.0", LOG_DEBUG,
                        DLSYM_ARG(Tss2_MU_TPM2_CC_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_PRIVATE_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_PRIVATE_Unmarshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_PUBLIC_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_PUBLIC_Unmarshal),
                        DLSYM_ARG(Tss2_MU_TPML_PCR_SELECTION_Marshal),
                        DLSYM_ARG(Tss2_MU_TPMT_HA_Marshal),
                        DLSYM_ARG(Tss2_MU_TPMT_PUBLIC_Marshal));
}

static inline void Esys_Freep(void *p) {
        if (*(void**) p)
                sym_Esys_Free(*(void**) p);
}

/* Get a specific TPM capability (or capabilities).
 *
 * Returns 0 if there are no more capability properties of the requested type, or 1 if there are more, or < 0
 * on any error. Both 0 and 1 indicate this completed successfully, but do not indicate how many capability
 * properties were provided in 'ret_capability_data'. To find the number of provided properties, check the
 * specific type's 'count' field (e.g. for TPM2_CAP_ALGS, check ret_capability_data->algorithms.count).
 *
 * This calls TPM2_GetCapability() and does not alter the provided data, so it is important to understand how
 * that TPM function works. It is recommended to check the TCG TPM specification Part 3 ("Commands") section
 * on TPM2_GetCapability() for full details, but a short summary is: if this returns 0, all available
 * properties have been provided in ret_capability_data, or no properties were available. If this returns 1,
 * there are between 1 and "count" properties provided in ret_capability_data, and there are more available.
 * Note that this may provide less than "count" properties even if the TPM has more available. Also, each
 * capability category may have more specific requirements than described here; see the spec for exact
 * details. */
static int tpm2_get_capability(
                Tpm2Context *c,
                TPM2_CAP capability,
                uint32_t property,
                uint32_t count,
                TPMU_CAPABILITIES *ret_capability_data) {

        _cleanup_(Esys_Freep) TPMS_CAPABILITY_DATA *capabilities = NULL;
        TPMI_YES_NO more;
        TSS2_RC rc;

        assert(c);

        log_debug("Getting TPM2 capability 0x%04" PRIx32 " property 0x%04" PRIx32 " count %" PRIu32 ".",
                  capability, property, count);

        rc = sym_Esys_GetCapability(
                        c->esys_context,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        capability,
                        property,
                        count,
                        &more,
                        &capabilities);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to get TPM2 capability 0x%04" PRIx32 " property 0x%04" PRIx32 ": %s",
                                       capability, property, sym_Tss2_RC_Decode(rc));

        if (capabilities->capability != capability)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "TPM provided wrong capability: 0x%04" PRIx32 " instead of 0x%04" PRIx32 ".",
                                       capabilities->capability, capability);

        if (ret_capability_data)
                *ret_capability_data = capabilities->data;

        return more == TPM2_YES;
}

#define TPMA_CC_TO_TPM2_CC(cca) (((cca) & TPMA_CC_COMMANDINDEX_MASK) >> TPMA_CC_COMMANDINDEX_SHIFT)

static int tpm2_cache_capabilities(Tpm2Context *c) {
        TPMU_CAPABILITIES capability;
        int r;

        assert(c);

        /* Cache the algorithms. The spec indicates supported algorithms can only be modified during runtime
         * by the SetAlgorithmSet() command. Unfortunately, the spec doesn't require a TPM reinitialization
         * after changing the algorithm set (unless the PCR algorithms are changed). However, the spec also
         * indicates the TPM behavior after SetAlgorithmSet() is "vendor-dependent", giving the example of
         * flushing sessions and objects, erasing policies, etc. So, if the algorithm set is programmatically
         * changed while we are performing some operation, it's reasonable to assume it will break us even if
         * we don't cache the algorithms, thus they should be "safe" to cache. */
        TPM2_ALG_ID current_alg = TPM2_ALG_FIRST;
        for (;;) {
                r = tpm2_get_capability(
                                c,
                                TPM2_CAP_ALGS,
                                (uint32_t) current_alg, /* The spec states to cast TPM2_ALG_ID to uint32_t. */
                                TPM2_MAX_CAP_ALGS,
                                &capability);
                if (r < 0)
                        return r;

                TPML_ALG_PROPERTY algorithms = capability.algorithms;

                /* We should never get 0; the TPM must support some algorithms, and it must not set 'more' if
                 * there are no more. */
                assert(algorithms.count > 0);

                if (!GREEDY_REALLOC_APPEND(
                                c->capability_algorithms,
                                c->n_capability_algorithms,
                                algorithms.algProperties,
                                algorithms.count))
                        return log_oom();

                if (r == 0)
                        break;

                /* Set current_alg to alg id after last alg id the TPM provided */
                current_alg = algorithms.algProperties[algorithms.count - 1].alg + 1;
        }

        /* Cache the command capabilities. The spec isn't actually clear if commands can be added/removed
         * while running, but that would be crazy, so let's hope it is not possible. */
        TPM2_CC current_cc = TPM2_CC_FIRST;
        for (;;) {
                r = tpm2_get_capability(
                                c,
                                TPM2_CAP_COMMANDS,
                                current_cc,
                                TPM2_MAX_CAP_CC,
                                &capability);
                if (r < 0)
                        return r;

                TPML_CCA commands = capability.command;

                /* We should never get 0; the TPM must support some commands, and it must not set 'more' if
                 * there are no more. */
                assert(commands.count > 0);

                if (!GREEDY_REALLOC_APPEND(
                                c->capability_commands,
                                c->n_capability_commands,
                                commands.commandAttributes,
                                commands.count))
                        return log_oom();

                if (r == 0)
                        break;

                /* Set current_cc to index after last cc the TPM provided */
                current_cc = TPMA_CC_TO_TPM2_CC(commands.commandAttributes[commands.count - 1]) + 1;
        }

        /* Cache the PCR capabilities, which are safe to cache, as the only way they can change is
         * TPM2_PCR_Allocate(), which changes the allocation after the next _TPM_Init(). If the TPM is
         * reinitialized while we are using it, all our context and sessions will be invalid, so we can
         * safely assume the TPM PCR allocation will not change while we are using it. */
        r = tpm2_get_capability(
                        c,
                        TPM2_CAP_PCRS,
                        /* property= */ 0,
                        /* count= */ 1,
                        &capability);
        if (r < 0)
                return r;
        if (r == 1)
                /* This should never happen. Part 3 ("Commands") of the TCG TPM2 spec in the section for
                 * TPM2_GetCapability states: "TPM_CAP_PCRS – Returns the current allocation of PCR in a
                 * TPML_PCR_SELECTION. The property parameter shall be zero. The TPM will always respond to
                 * this command with the full PCR allocation and moreData will be NO." */
                log_warning("TPM bug: reported multiple PCR sets; using only first set.");
        c->capability_pcrs = capability.assignedPCR;

        return 0;
}

/* Get the TPMA_ALGORITHM for a TPM2_ALG_ID. Returns true if the TPM supports the algorithm and the
 * TPMA_ALGORITHM is provided, otherwise false. */
static bool tpm2_get_capability_alg(Tpm2Context *c, TPM2_ALG_ID alg, TPMA_ALGORITHM *ret) {
        assert(c);

        FOREACH_ARRAY(alg_prop, c->capability_algorithms, c->n_capability_algorithms)
                if (alg_prop->alg == alg) {
                        if (ret)
                                *ret = alg_prop->algProperties;
                        return true;
                }

        log_debug("TPM does not support alg 0x%02" PRIx16 ".", alg);
        if (ret)
                *ret = 0;

        return false;
}

bool tpm2_supports_alg(Tpm2Context *c, TPM2_ALG_ID alg) {
        return tpm2_get_capability_alg(c, alg, NULL);
}

/* Get the TPMA_CC for a TPM2_CC. Returns true if the TPM supports the command and the TPMA_CC is provided,
 * otherwise false. */
static bool tpm2_get_capability_command(Tpm2Context *c, TPM2_CC command, TPMA_CC *ret) {
        assert(c);

        FOREACH_ARRAY(cca, c->capability_commands, c->n_capability_commands)
                if (TPMA_CC_TO_TPM2_CC(*cca) == command) {
                        if (ret)
                                *ret = *cca;
                        return true;
                }

        log_debug("TPM does not support command 0x%04" PRIx32 ".", command);
        if (ret)
                *ret = 0;

        return false;
}

bool tpm2_supports_command(Tpm2Context *c, TPM2_CC command) {
        return tpm2_get_capability_command(c, command, NULL);
}

/* Returns 1 if the TPM supports the ECC curve, 0 if not, or < 0 for any error. */
static int tpm2_supports_ecc_curve(Tpm2Context *c, TPM2_ECC_CURVE curve) {
        TPMU_CAPABILITIES capability;
        int r;

        /* The spec explicitly states the TPM2_ECC_CURVE should be cast to uint32_t. */
        r = tpm2_get_capability(c, TPM2_CAP_ECC_CURVES, (uint32_t) curve, 1, &capability);
        if (r < 0)
                return r;

        TPML_ECC_CURVE eccCurves = capability.eccCurves;
        if (eccCurves.count == 0 || eccCurves.eccCurves[0] != curve) {
                log_debug("TPM does not support ECC curve 0x%02" PRIx16 ".", curve);
                return 0;
        }

        return 1;
}

/* Query the TPM for populated handles.
 *
 * This provides an array of handle indexes populated in the TPM, starting at the requested handle. The array will
 * contain only populated handle addresses (which might not include the requested handle). The number of
 * handles will be no more than the 'max' number requested. This will not search past the end of the handle
 * range (i.e. handle & 0xff000000).
 *
 * Returns 0 if all populated handles in the range (starting at the requested handle) were provided (or no
 * handles were in the range), or 1 if there are more populated handles in the range, or < 0 on any error. */
static int tpm2_get_capability_handles(
                Tpm2Context *c,
                TPM2_HANDLE start,
                size_t max,
                TPM2_HANDLE **ret_handles,
                size_t *ret_n_handles) {

        _cleanup_free_ TPM2_HANDLE *handles = NULL;
        size_t n_handles = 0;
        TPM2_HANDLE current = start;
        int r = 0;

        assert(c);
        assert(ret_handles);
        assert(ret_n_handles);

        while (max > 0) {
                TPMU_CAPABILITIES capability;
                r = tpm2_get_capability(c, TPM2_CAP_HANDLES, current, (uint32_t) max, &capability);
                if (r < 0)
                        return r;

                TPML_HANDLE handle_list = capability.handles;
                if (handle_list.count == 0)
                        break;

                assert(handle_list.count <= max);

                if (n_handles > SIZE_MAX - handle_list.count)
                        return log_oom();

                if (!GREEDY_REALLOC(handles, n_handles + handle_list.count))
                        return log_oom();

                memcpy_safe(&handles[n_handles], handle_list.handle, sizeof(handles[0]) * handle_list.count);

                max -= handle_list.count;
                n_handles += handle_list.count;

                /* Update current to the handle index after the last handle in the list. */
                current = handles[n_handles - 1] + 1;

                if (r == 0)
                        /* No more handles in this range. */
                        break;
        }

        *ret_handles = TAKE_PTR(handles);
        *ret_n_handles = n_handles;

        return r;
}

#define TPM2_HANDLE_RANGE(h) ((TPM2_HANDLE)((h) & TPM2_HR_RANGE_MASK))
#define TPM2_HANDLE_TYPE(h) ((TPM2_HT)(TPM2_HANDLE_RANGE(h) >> TPM2_HR_SHIFT))

/* Returns 1 if the handle is populated in the TPM, 0 if not, and < 0 on any error. */
static int tpm2_get_capability_handle(Tpm2Context *c, TPM2_HANDLE handle) {
        _cleanup_free_ TPM2_HANDLE *handles = NULL;
        size_t n_handles = 0;
        int r;

        r = tpm2_get_capability_handles(c, handle, 1, &handles, &n_handles);
        if (r < 0)
                return r;

        return n_handles == 0 ? false : handles[0] == handle;
}

/* Returns 1 if the TPM supports the parms, or 0 if the TPM does not support the parms. */
bool tpm2_test_parms(Tpm2Context *c, TPMI_ALG_PUBLIC alg, const TPMU_PUBLIC_PARMS *parms) {
        TSS2_RC rc;

        assert(c);
        assert(parms);

        TPMT_PUBLIC_PARMS parameters = {
                .type = alg,
                .parameters = *parms,
        };

        rc = sym_Esys_TestParms(c->esys_context, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &parameters);
        if (rc != TSS2_RC_SUCCESS)
                /* The spec says if the parms are not supported the TPM returns "...the appropriate
                 * unmarshaling error if a parameter is not valid". Since the spec (currently) defines 15
                 * unmarshaling errors, instead of checking for them all here, let's just assume any error
                 * indicates unsupported parms, and log the specific error text. */
                log_debug("TPM does not support tested parms: %s", sym_Tss2_RC_Decode(rc));

        return rc == TSS2_RC_SUCCESS;
}

static inline bool tpm2_supports_tpmt_public(Tpm2Context *c, const TPMT_PUBLIC *public) {
        assert(c);
        assert(public);

        return tpm2_test_parms(c, public->type, &public->parameters);
}

static inline bool tpm2_supports_tpmt_sym_def_object(Tpm2Context *c, const TPMT_SYM_DEF_OBJECT *parameters) {
        assert(c);
        assert(parameters);

        TPMU_PUBLIC_PARMS parms = {
                .symDetail.sym = *parameters,
        };

        return tpm2_test_parms(c, TPM2_ALG_SYMCIPHER, &parms);
}

static inline bool tpm2_supports_tpmt_sym_def(Tpm2Context *c, const TPMT_SYM_DEF *parameters) {
        assert(c);
        assert(parameters);

        /* Unfortunately, TPMT_SYM_DEF and TPMT_SYM_DEF_OBEJECT are separately defined, even though they are
         * functionally identical. */
        TPMT_SYM_DEF_OBJECT object = {
                .algorithm = parameters->algorithm,
                .keyBits = parameters->keyBits,
                .mode = parameters->mode,
        };

        return tpm2_supports_tpmt_sym_def_object(c, &object);
}

static Tpm2Context *tpm2_context_free(Tpm2Context *c) {
        if (!c)
                return NULL;

        if (c->esys_context)
                sym_Esys_Finalize(&c->esys_context);

        c->tcti_context = mfree(c->tcti_context);
        c->tcti_dl = safe_dlclose(c->tcti_dl);

        c->capability_algorithms = mfree(c->capability_algorithms);
        c->capability_commands = mfree(c->capability_commands);

        return mfree(c);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(Tpm2Context, tpm2_context, tpm2_context_free);

static const TPMT_SYM_DEF SESSION_TEMPLATE_SYM_AES_128_CFB = {
        .algorithm = TPM2_ALG_AES,
        .keyBits.aes = 128,
        .mode.aes = TPM2_ALG_CFB, /* The spec requires sessions to use CFB. */
};

int tpm2_context_new(const char *device, Tpm2Context **ret_context) {
        _cleanup_(tpm2_context_unrefp) Tpm2Context *context = NULL;
        TSS2_RC rc;
        int r;

        assert(ret_context);

        context = new(Tpm2Context, 1);
        if (!context)
                return log_oom();

        *context = (Tpm2Context) {
                .n_ref = 1,
        };

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        if (!device) {
                device = secure_getenv("SYSTEMD_TPM2_DEVICE");
                if (device)
                        /* Setting the env var to an empty string forces tpm2-tss' own device picking
                         * logic to be used. */
                        device = empty_to_null(device);
                else
                        /* If nothing was specified explicitly, we'll use a hardcoded default: the "device" tcti
                         * driver and the "/dev/tpmrm0" device. We do this since on some distributions the tpm2-abrmd
                         * might be used and we really don't want that, since it is a system service and that creates
                         * various ordering issues/deadlocks during early boot. */
                        device = "device:/dev/tpmrm0";
        }

        if (device) {
                const char *param, *driver, *fn;
                const TSS2_TCTI_INFO* info;
                TSS2_TCTI_INFO_FUNC func;
                size_t sz = 0;

                param = strchr(device, ':');
                if (param) {
                        /* Syntax #1: Pair of driver string and arbitrary parameter */
                        driver = strndupa_safe(device, param - device);
                        if (isempty(driver))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 driver name is empty, refusing.");

                        param++;
                } else if (path_is_absolute(device) && path_is_valid(device)) {
                        /* Syntax #2: TPM device node */
                        driver = "device";
                        param = device;
                } else
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid TPM2 driver string, refusing.");

                log_debug("Using TPM2 TCTI driver '%s' with device '%s'.", driver, param);

                fn = strjoina("libtss2-tcti-", driver, ".so.0");

                /* Better safe than sorry, let's refuse strings that cannot possibly be valid driver early, before going to disk. */
                if (!filename_is_valid(fn))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 driver name '%s' not valid, refusing.", driver);

                context->tcti_dl = dlopen(fn, RTLD_NOW|RTLD_NODELETE);
                if (!context->tcti_dl)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Failed to load %s: %s", fn, dlerror());

                log_debug("Loaded '%s' via dlopen()", fn);

                func = dlsym(context->tcti_dl, TSS2_TCTI_INFO_SYMBOL);
                if (!func)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to find TCTI info symbol " TSS2_TCTI_INFO_SYMBOL ": %s",
                                               dlerror());

                info = func();
                if (!info)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Unable to get TCTI info data.");

                log_debug("Loaded TCTI module '%s' (%s) [Version %" PRIu32 "]", info->name, info->description, info->version);

                rc = info->init(NULL, &sz, NULL);
                if (rc != TPM2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to initialize TCTI context: %s", sym_Tss2_RC_Decode(rc));

                context->tcti_context = malloc0(sz);
                if (!context->tcti_context)
                        return log_oom();

                rc = info->init(context->tcti_context, &sz, param);
                if (rc != TPM2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to initialize TCTI context: %s", sym_Tss2_RC_Decode(rc));
        }

        rc = sym_Esys_Initialize(&context->esys_context, context->tcti_context, NULL);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to initialize TPM context: %s", sym_Tss2_RC_Decode(rc));

        rc = sym_Esys_Startup(context->esys_context, TPM2_SU_CLEAR);
        if (rc == TPM2_RC_INITIALIZE)
                log_debug("TPM already started up.");
        else if (rc == TSS2_RC_SUCCESS)
                log_debug("TPM successfully started up.");
        else
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to start up TPM: %s", sym_Tss2_RC_Decode(rc));

        r = tpm2_cache_capabilities(context);
        if (r < 0)
                return r;

        /* We require AES and CFB support for session encryption. */
        if (!tpm2_supports_alg(context, TPM2_ALG_AES))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM does not support AES.");

        if (!tpm2_supports_alg(context, TPM2_ALG_CFB))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM does not support CFB.");

        if (!tpm2_supports_tpmt_sym_def(context, &SESSION_TEMPLATE_SYM_AES_128_CFB))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM does not support AES-128-CFB.");

        *ret_context = TAKE_PTR(context);

        return 0;
}

static void tpm2_handle_cleanup(ESYS_CONTEXT *esys_context, ESYS_TR esys_handle, bool flush) {
        TSS2_RC rc;

        if (!esys_context || esys_handle == ESYS_TR_NONE)
                return;

        /* Closing the handle removes its reference from the esys_context, but leaves the corresponding
         * handle in the actual TPM. Flushing the handle removes its reference from the esys_context as well
         * as removing its corresponding handle from the actual TPM. */
        if (flush)
                rc = sym_Esys_FlushContext(esys_context, esys_handle);
        else
                rc = sym_Esys_TR_Close(esys_context, &esys_handle);
        if (rc != TSS2_RC_SUCCESS) /* We ignore failures here (besides debug logging), since this is called
                                    * in error paths, where we cannot do anything about failures anymore. And
                                    * when it is called in successful codepaths by this time we already did
                                    * what we wanted to do, and got the results we wanted so there's no
                                    * reason to make this fail more loudly than necessary. */
                log_debug("Failed to %s TPM handle, ignoring: %s", flush ? "flush" : "close", sym_Tss2_RC_Decode(rc));
}

Tpm2Handle *tpm2_handle_free(Tpm2Handle *handle) {
        if (!handle)
                return NULL;

        _cleanup_(tpm2_context_unrefp) Tpm2Context *context = (Tpm2Context*)handle->tpm2_context;
        if (context)
                tpm2_handle_cleanup(context->esys_context, handle->esys_handle, handle->flush);

        return mfree(handle);
}

int tpm2_handle_new(Tpm2Context *context, Tpm2Handle **ret_handle) {
        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;

        assert(ret_handle);

        handle = new(Tpm2Handle, 1);
        if (!handle)
                return log_oom();

        *handle = (Tpm2Handle) {
                .tpm2_context = tpm2_context_ref(context),
                .esys_handle = ESYS_TR_NONE,
                .flush = true,
        };

        *ret_handle = TAKE_PTR(handle);

        return 0;
}

/* Create a Tpm2Handle object that references a pre-existing handle in the TPM, at the TPM2_HANDLE address
 * provided. This should be used only for persistent, transient, or NV handles. Returns 1 on success, 0 if
 * the requested handle is not present in the TPM, or < 0 on error. */
static int tpm2_esys_handle_from_tpm_handle(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2_HANDLE tpm_handle,
                Tpm2Handle **ret_handle) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(tpm_handle > 0);
        assert(ret_handle);

        /* Let's restrict this, at least for now, to allow only some handle types. */
        switch (TPM2_HANDLE_TYPE(tpm_handle)) {
        case TPM2_HT_PERSISTENT:
        case TPM2_HT_NV_INDEX:
        case TPM2_HT_TRANSIENT:
                break;
        case TPM2_HT_PCR:
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Refusing to create ESYS handle for PCR handle 0x%08" PRIx32 ".",
                                       tpm_handle);
        case TPM2_HT_HMAC_SESSION:
        case TPM2_HT_POLICY_SESSION:
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Refusing to create ESYS handle for session handle 0x%08" PRIx32 ".",
                                       tpm_handle);
        case TPM2_HT_PERMANENT: /* Permanent handles are defined, e.g. ESYS_TR_RH_OWNER. */
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Refusing to create ESYS handle for permanent handle 0x%08" PRIx32 ".",
                                       tpm_handle);
        default:
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Refusing to create ESYS handle for unknown handle 0x%08" PRIx32 ".",
                                       tpm_handle);
        }

        r = tpm2_get_capability_handle(c, tpm_handle);
        if (r < 0)
                return r;
        if (r == 0) {
                log_debug("TPM handle 0x%08" PRIx32 " not populated.", tpm_handle);
                *ret_handle = NULL;
                return 0;
        }

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        /* Since we didn't create this handle in the TPM (this is only creating an ESYS_TR handle for the
         * pre-existing TPM handle), we shouldn't flush (or evict) it on cleanup. */
        handle->flush = false;

        rc = sym_Esys_TR_FromTPMPublic(
                        c->esys_context,
                        tpm_handle,
                        session ? session->esys_handle : ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &handle->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to read public info: %s", sym_Tss2_RC_Decode(rc));

        *ret_handle = TAKE_PTR(handle);

        return 1;
}

/* Copy an object in the TPM at a transient location to a persistent location.
 *
 * The provided transient handle must exist in the TPM in the transient range. The persistent location may be
 * 0 or any location in the persistent range. If 0, this will try each handle in the persistent range, in
 * ascending order, until an available one is found. If non-zero, only the requested persistent location will
 * be used.
 *
 * Returns 1 if the object was successfully persisted, or 0 if there is already a key at the requested
 * location(s), or < 0 on error. The persistent handle is only provided when returning 1. */
static int tpm2_persist_handle(
                Tpm2Context *c,
                const Tpm2Handle *transient_handle,
                const Tpm2Handle *session,
                TPMI_DH_PERSISTENT persistent_location,
                Tpm2Handle **ret_persistent_handle) {

        /* We don't use TPM2_PERSISTENT_FIRST and TPM2_PERSISTENT_LAST here due to:
         * https://github.com/systemd/systemd/pull/27713#issuecomment-1591864753 */
        TPMI_DH_PERSISTENT first = UINT32_C(0x81000000), last = UINT32_C(0x81ffffff);
        TSS2_RC rc;
        int r;

        assert(c);
        assert(transient_handle);

        /* If persistent location specified, only try that. */
        if (persistent_location != 0) {
                if (TPM2_HANDLE_TYPE(persistent_location) != TPM2_HT_PERSISTENT)
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Handle not in persistent range: 0x%x", persistent_location);

                first = last = persistent_location;
        }

        for (TPMI_DH_PERSISTENT requested = first; requested <= last; requested++) {
                _cleanup_(tpm2_handle_freep) Tpm2Handle *persistent_handle = NULL;
                r = tpm2_handle_new(c, &persistent_handle);
                if (r < 0)
                        return r;

                /* Since this is a persistent handle, don't flush it. */
                persistent_handle->flush = false;

                rc = sym_Esys_EvictControl(
                                c->esys_context,
                                ESYS_TR_RH_OWNER,
                                transient_handle->esys_handle,
                                session ? session->esys_handle : ESYS_TR_PASSWORD,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                requested,
                                &persistent_handle->esys_handle);
                if (rc == TSS2_RC_SUCCESS) {
                        if (ret_persistent_handle)
                                *ret_persistent_handle = TAKE_PTR(persistent_handle);

                        return 1;
                }
                if (rc != TPM2_RC_NV_DEFINED)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to persist handle: %s", sym_Tss2_RC_Decode(rc));
        }

        if (ret_persistent_handle)
                *ret_persistent_handle = NULL;

        return 0;
}

#define TPM2_CREDIT_RANDOM_FLAG_PATH "/run/systemd/tpm-rng-credited"

static int tpm2_credit_random(Tpm2Context *c) {
        size_t rps, done = 0;
        TSS2_RC rc;
        usec_t t;
        int r;

        assert(c);

        /* Pulls some entropy from the TPM and adds it into the kernel RNG pool. That way we can say that the
         * key we will ultimately generate with the kernel random pool is at least as good as the TPM's RNG,
         * but likely better. Note that we don't trust the TPM RNG very much, hence do not actually credit
         * any entropy. */

        if (access(TPM2_CREDIT_RANDOM_FLAG_PATH, F_OK) < 0) {
                if (errno != ENOENT)
                        log_debug_errno(errno, "Failed to detect if '" TPM2_CREDIT_RANDOM_FLAG_PATH "' exists, ignoring: %m");
        } else {
                log_debug("Not adding TPM2 entropy to the kernel random pool again.");
                return 0; /* Already done */
        }

        t = now(CLOCK_MONOTONIC);

        for (rps = random_pool_size(); rps > 0;) {
                _cleanup_(Esys_Freep) TPM2B_DIGEST *buffer = NULL;

                rc = sym_Esys_GetRandom(
                                c->esys_context,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                MIN(rps, 32U), /* 32 is supposedly a safe choice, given that AES 256bit keys are this long, and TPM2 baseline requires support for those. */
                                &buffer);
                if (rc != TSS2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to acquire entropy from TPM: %s", sym_Tss2_RC_Decode(rc));

                if (buffer->size == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Zero-sized entropy returned from TPM.");

                r = random_write_entropy(-1, buffer->buffer, buffer->size, /* credit= */ false);
                if (r < 0)
                        return log_error_errno(r, "Failed wo write entropy to kernel: %m");

                done += buffer->size;
                rps = LESS_BY(rps, buffer->size);
        }

        log_debug("Added %zu bytes of TPM2 entropy to the kernel random pool in %s.", done, FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - t, 0));

        r = touch(TPM2_CREDIT_RANDOM_FLAG_PATH);
        if (r < 0)
                log_debug_errno(r, "Failed to touch '" TPM2_CREDIT_RANDOM_FLAG_PATH "', ignoring: %m");

        return 0;
}

static int tpm2_read_public(
                Tpm2Context *c,
                const Tpm2Handle *session,
                const Tpm2Handle *handle,
                TPM2B_PUBLIC **ret_public,
                TPM2B_NAME **ret_name,
                TPM2B_NAME **ret_qname) {

        TSS2_RC rc;

        assert(c);
        assert(handle);

        rc = sym_Esys_ReadPublic(
                        c->esys_context,
                        handle->esys_handle,
                        session ? session->esys_handle : ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ret_public,
                        ret_name,
                        ret_qname);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to read public info: %s", sym_Tss2_RC_Decode(rc));

        return 0;
}

/* Get one of the legacy primary key templates.
 *
 * The legacy templates should only be used for older sealed data that did not use the SRK. Instead of a
 * persistent SRK, a transient key was created to seal the data and then flushed; and the exact same template
 * must be used to recreate the same transient key to unseal the data. The alg parameter must be TPM2_ALG_RSA
 * or TPM2_ALG_ECC. This does not check if the alg is actually supported on this TPM. */
static int tpm2_get_legacy_template(TPMI_ALG_PUBLIC alg, TPMT_PUBLIC *ret_template) {
        /* Do not modify. */
        static const TPMT_PUBLIC legacy_ecc = {
                .type = TPM2_ALG_ECC,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = TPMA_OBJECT_RESTRICTED|TPMA_OBJECT_DECRYPT|TPMA_OBJECT_FIXEDTPM|TPMA_OBJECT_FIXEDPARENT|TPMA_OBJECT_SENSITIVEDATAORIGIN|TPMA_OBJECT_USERWITHAUTH,
                .parameters.eccDetail = {
                        .symmetric = {
                                .algorithm = TPM2_ALG_AES,
                                .keyBits.aes = 128,
                                .mode.aes = TPM2_ALG_CFB,
                        },
                        .scheme.scheme = TPM2_ALG_NULL,
                        .curveID = TPM2_ECC_NIST_P256,
                        .kdf.scheme = TPM2_ALG_NULL,
                },
        };

        /* Do not modify. */
        static const TPMT_PUBLIC legacy_rsa = {
                .type = TPM2_ALG_RSA,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = TPMA_OBJECT_RESTRICTED|TPMA_OBJECT_DECRYPT|TPMA_OBJECT_FIXEDTPM|TPMA_OBJECT_FIXEDPARENT|TPMA_OBJECT_SENSITIVEDATAORIGIN|TPMA_OBJECT_USERWITHAUTH,
                .parameters.rsaDetail = {
                        .symmetric = {
                                .algorithm = TPM2_ALG_AES,
                                .keyBits.aes = 128,
                                .mode.aes = TPM2_ALG_CFB,
                        },
                        .scheme.scheme = TPM2_ALG_NULL,
                        .keyBits = 2048,
                },
        };

        assert(ret_template);

        if (alg == TPM2_ALG_ECC)
                *ret_template = legacy_ecc;
        else if (alg == TPM2_ALG_RSA)
                *ret_template = legacy_rsa;
        else
                return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Unsupported legacy SRK alg: 0x%x", alg);

        return 0;
}

/* Get a Storage Root Key (SRK) template.
 *
 * The SRK template values are recommended by the "TCG TPM v2.0 Provisioning Guidance" document in section
 * 7.5.1 "Storage Primary Key (SRK) Templates", referencing "TCG EK Credential Profile for TPM Family 2.0".
 * The EK Credential Profile version 2.0 provides only a single template each for RSA and ECC, while later EK
 * Credential Profile versions provide more templates, and keep the original templates as "L-1" (for RSA) and
 * "L-2" (for ECC).
 *
 * https://trustedcomputinggroup.org/resource/tcg-tpm-v2-0-provisioning-guidance
 * https://trustedcomputinggroup.org/resource/http-trustedcomputinggroup-org-wp-content-uploads-tcg-ek-credential-profile
 *
 * These templates are only needed to create a new persistent SRK (or a new transient key that is
 * SRK-compatible). Preferably, the TPM should contain a shared SRK located at the reserved shared SRK handle
 * (see TPM2_SRK_HANDLE and tpm2_get_srk() below).
 *
 * The alg must be TPM2_ALG_RSA or TPM2_ALG_ECC. Returns error if the requested template is not supported on
 * this TPM. Also see tpm2_get_best_srk_template() below. */
static int tpm2_get_srk_template(Tpm2Context *c, TPMI_ALG_PUBLIC alg, TPMT_PUBLIC *ret_template) {
        /* The attributes are the same between ECC and RSA templates. This has the changes specified in the
         * Provisioning Guidance document, specifically:
         * TPMA_OBJECT_USERWITHAUTH is added.
         * TPMA_OBJECT_ADMINWITHPOLICY is removed.
         * TPMA_OBJECT_NODA is added. */
        TPMA_OBJECT srk_attributes =
                        TPMA_OBJECT_DECRYPT |
                        TPMA_OBJECT_FIXEDPARENT |
                        TPMA_OBJECT_FIXEDTPM |
                        TPMA_OBJECT_NODA |
                        TPMA_OBJECT_RESTRICTED |
                        TPMA_OBJECT_SENSITIVEDATAORIGIN |
                        TPMA_OBJECT_USERWITHAUTH;

        /* The symmetric configuration is the same between ECC and RSA templates. */
        TPMT_SYM_DEF_OBJECT srk_symmetric = {
                .algorithm = TPM2_ALG_AES,
                .keyBits.aes = 128,
                .mode.aes = TPM2_ALG_CFB,
        };

        /* Both templates have an empty authPolicy as specified by the Provisioning Guidance document. */

        /* From the EK Credential Profile template "L-2". */
        TPMT_PUBLIC srk_ecc = {
                .type = TPM2_ALG_ECC,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = srk_attributes,
                .parameters.eccDetail = {
                        .symmetric = srk_symmetric,
                        .scheme.scheme = TPM2_ALG_NULL,
                        .curveID = TPM2_ECC_NIST_P256,
                        .kdf.scheme = TPM2_ALG_NULL,
                },
        };

        /* From the EK Credential Profile template "L-1". */
        TPMT_PUBLIC srk_rsa = {
                .type = TPM2_ALG_RSA,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = srk_attributes,
                .parameters.rsaDetail = {
                        .symmetric = srk_symmetric,
                        .scheme.scheme = TPM2_ALG_NULL,
                        .keyBits = 2048,
                },
        };

        assert(c);
        assert(ret_template);

        if (alg == TPM2_ALG_ECC) {
                if (!tpm2_supports_alg(c, TPM2_ALG_ECC))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support ECC.");

                if (!tpm2_supports_ecc_curve(c, srk_ecc.parameters.eccDetail.curveID))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support ECC-NIST-P256 curve.");

                if (!tpm2_supports_tpmt_public(c, &srk_ecc))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support SRK ECC template L-2.");

                *ret_template = srk_ecc;
                return 0;
        }

        if (alg == TPM2_ALG_RSA) {
                if (!tpm2_supports_alg(c, TPM2_ALG_RSA))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support RSA.");

                if (!tpm2_supports_tpmt_public(c, &srk_rsa))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support SRK RSA template L-1.");

                *ret_template = srk_rsa;
                return 0;
        }

        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Unsupported SRK alg: 0x%x.", alg);
}

/* Get the best supported SRK template. ECC is preferred, then RSA. */
static int tpm2_get_best_srk_template(Tpm2Context *c, TPMT_PUBLIC *ret_template) {
        if (tpm2_get_srk_template(c, TPM2_ALG_ECC, ret_template) >= 0 ||
            tpm2_get_srk_template(c, TPM2_ALG_RSA, ret_template) >= 0)
                return 0;

        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "TPM does not support either SRK template L-1 (RSA) or L-2 (ECC).");
}

/* The SRK handle is defined in the Provisioning Guidance document (see above) in the table "Reserved Handles
 * for TPM Provisioning Fundamental Elements". The SRK is useful because it is "shared", meaning it has no
 * authValue nor authPolicy set, and thus may be used by anyone on the system to generate derived keys or
 * seal secrets. This is useful if the TPM has an auth (password) set for the 'owner hierarchy', which would
 * prevent users from generating primary transient keys, unless they knew the owner hierarchy auth. See
 * the Provisioning Guidance document for more details. */
#define TPM2_SRK_HANDLE UINT32_C(0x81000001)

/* Get the SRK. Returns 1 if SRK is found, 0 if there is no SRK, or < 0 on error. Also see
 * tpm2_get_or_create_srk() below. */
static int tpm2_get_srk(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2B_PUBLIC **ret_public,
                TPM2B_NAME **ret_name,
                TPM2B_NAME **ret_qname,
                Tpm2Handle **ret_handle) {

        int r;

        assert(c);

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_esys_handle_from_tpm_handle(c, session, TPM2_SRK_HANDLE, &handle);
        if (r < 0)
                return r;
        if (r == 0) { /* SRK not found */
                if (ret_public)
                        *ret_public = NULL;
                if (ret_name)
                        *ret_name = NULL;
                if (ret_qname)
                        *ret_qname = NULL;
                if (ret_handle)
                        *ret_handle = NULL;
                return 0;
        }

        if (ret_public || ret_name || ret_qname) {
                r = tpm2_read_public(c, session, handle, ret_public, ret_name, ret_qname);
                if (r < 0)
                        return r;
        }

        if (ret_handle)
                *ret_handle = TAKE_PTR(handle);

        return 1;
}

/* Get the SRK, creating one if needed. Returns 0 on success, or < 0 on error. */
static int tpm2_get_or_create_srk(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2B_PUBLIC **ret_public,
                TPM2B_NAME **ret_name,
                TPM2B_NAME **ret_qname,
                Tpm2Handle **ret_handle) {

        int r;

        r = tpm2_get_srk(c, session, ret_public, ret_name, ret_qname, ret_handle);
        if (r < 0)
                return r;
        if (r == 1)
                return 0;

        /* No SRK, create and persist one */
        TPM2B_PUBLIC template = { .size = sizeof(TPMT_PUBLIC), };
        r = tpm2_get_best_srk_template(c, &template.publicArea);
        if (r < 0)
                return log_error_errno(r, "Could not get best SRK template: %m");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *transient_handle = NULL;
        r = tpm2_create_primary(
                        c,
                        session,
                        &template,
                        /* sensitive= */ NULL,
                        /* ret_public= */ NULL,
                        &transient_handle);
        if (r < 0)
                return r;

        /* Try to persist the transient SRK we created. No locking needed; if multiple threads are trying to
         * persist SRKs concurrently, only one will succeed (r == 1) while the rest will fail (r == 0). In
         * either case, all threads will get the persistent SRK below. */
        r = tpm2_persist_handle(c, transient_handle, session, TPM2_SRK_HANDLE, /* ret_persistent_handle= */ NULL);
        if (r < 0)
                return r;

        /* The SRK should exist now. */
        r = tpm2_get_srk(c, session, ret_public, ret_name, ret_qname, ret_handle);
        if (r < 0)
                return r;
        if (r == 0)
                /* This should never happen. */
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "SRK we just persisted couldn't be found.");

        return 0;
}

/* Utility functions for TPMS_PCR_SELECTION. */

/* Convert a TPMS_PCR_SELECTION object to a mask. */
void tpm2_tpms_pcr_selection_to_mask(const TPMS_PCR_SELECTION *s, uint32_t *ret) {
        assert(s);
        assert(s->sizeofSelect <= sizeof(s->pcrSelect));
        assert(ret);

        uint32_t mask = 0;
        for (unsigned i = 0; i < s->sizeofSelect; i++)
                SET_FLAG(mask, (uint32_t)s->pcrSelect[i] << (i * 8), true);
        *ret = mask;
}

/* Convert a mask and hash alg to a TPMS_PCR_SELECTION object. */
void tpm2_tpms_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash_alg, TPMS_PCR_SELECTION *ret) {
        assert(ret);

        /* This is currently hardcoded at 24 PCRs, above. */
        if (!TPM2_PCR_MASK_VALID(mask))
                log_warning("PCR mask selections (%x) out of range, ignoring.",
                            mask & ~((uint32_t)TPM2_PCRS_MASK));

        *ret = (TPMS_PCR_SELECTION){
                .hash = hash_alg,
                .sizeofSelect = TPM2_PCRS_MAX / 8,
                .pcrSelect[0] = mask & 0xff,
                .pcrSelect[1] = (mask >> 8) & 0xff,
                .pcrSelect[2] = (mask >> 16) & 0xff,
        };
}

/* Add all PCR selections in 'b' to 'a'. Both must have the same hash alg. */
void tpm2_tpms_pcr_selection_add(TPMS_PCR_SELECTION *a, const TPMS_PCR_SELECTION *b) {
        assert(a);
        assert(b);
        assert(a->hash == b->hash);

        uint32_t maska, maskb;
        tpm2_tpms_pcr_selection_to_mask(a, &maska);
        tpm2_tpms_pcr_selection_to_mask(b, &maskb);
        tpm2_tpms_pcr_selection_from_mask(maska | maskb, a->hash, a);
}

/* Remove all PCR selections in 'b' from 'a'. Both must have the same hash alg. */
void tpm2_tpms_pcr_selection_sub(TPMS_PCR_SELECTION *a, const TPMS_PCR_SELECTION *b) {
        assert(a);
        assert(b);
        assert(a->hash == b->hash);

        uint32_t maska, maskb;
        tpm2_tpms_pcr_selection_to_mask(a, &maska);
        tpm2_tpms_pcr_selection_to_mask(b, &maskb);
        tpm2_tpms_pcr_selection_from_mask(maska & ~maskb, a->hash, a);
}

/* Move all PCR selections in 'b' to 'a'. Both must have the same hash alg. */
void tpm2_tpms_pcr_selection_move(TPMS_PCR_SELECTION *a, TPMS_PCR_SELECTION *b) {
        if (a == b)
                return;

        tpm2_tpms_pcr_selection_add(a, b);
        tpm2_tpms_pcr_selection_from_mask(0, b->hash, b);
}

#define FOREACH_PCR_IN_TPMS_PCR_SELECTION(pcr, tpms)                    \
        _FOREACH_PCR_IN_TPMS_PCR_SELECTION(pcr, tpms, UNIQ)
#define _FOREACH_PCR_IN_TPMS_PCR_SELECTION(pcr, tpms, uniq)             \
        FOREACH_PCR_IN_MASK(pcr,                                        \
                            ({ uint32_t UNIQ_T(_mask, uniq);            \
                                    tpm2_tpms_pcr_selection_to_mask(tpms, &UNIQ_T(_mask, uniq)); \
                                    UNIQ_T(_mask, uniq);                \
                            }))

#define FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml)    \
        UNIQ_FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml, UNIQ)
#define UNIQ_FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml, uniq) \
        for (TPML_PCR_SELECTION *UNIQ_T(_tpml, uniq) = (TPML_PCR_SELECTION*)(tpml); \
             UNIQ_T(_tpml, uniq); UNIQ_T(_tpml, uniq) = NULL)           \
                _FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, UNIQ_T(_tpml, uniq))
#define _FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml)   \
        for (TPMS_PCR_SELECTION *tpms = tpml->pcrSelections;            \
             (uint32_t)(tpms - tpml->pcrSelections) < tpml->count;      \
             tpms++)

#define FOREACH_PCR_IN_TPML_PCR_SELECTION(pcr, tpms, tpml)              \
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml)    \
                FOREACH_PCR_IN_TPMS_PCR_SELECTION(pcr, tpms)

char *tpm2_tpms_pcr_selection_to_string(const TPMS_PCR_SELECTION *s) {
        assert(s);

        const char *algstr = strna(tpm2_hash_alg_to_string(s->hash));

        uint32_t mask;
        tpm2_tpms_pcr_selection_to_mask(s, &mask);
        _cleanup_free_ char *maskstr = tpm2_pcr_mask_to_string(mask);
        if (!maskstr)
                return NULL;

        return strjoin(algstr, "(", maskstr, ")");
}

size_t tpm2_tpms_pcr_selection_weight(const TPMS_PCR_SELECTION *s) {
        assert(s);

        uint32_t mask;
        tpm2_tpms_pcr_selection_to_mask(s, &mask);
        return popcount(mask);
}

/* Utility functions for TPML_PCR_SELECTION. */

/* Remove the (0-based) index entry from 'l', shift all following entries, and update the count. */
static void tpm2_tpml_pcr_selection_remove_index(TPML_PCR_SELECTION *l, uint32_t index) {
        assert(l);
        assert(l->count <= ELEMENTSOF(l->pcrSelections));
        assert(index < l->count);

        size_t s = l->count - (index + 1);
        memmove(&l->pcrSelections[index], &l->pcrSelections[index + 1], s * sizeof(l->pcrSelections[0]));
        l->count--;
}

/* Get a TPMS_PCR_SELECTION from a TPML_PCR_SELECTION for the given hash alg. Returns NULL if there is no
 * entry for the hash alg. This guarantees the returned entry contains all the PCR selections for the given
 * hash alg, which may require modifying the TPML_PCR_SELECTION by removing duplicate entries. */
static TPMS_PCR_SELECTION *tpm2_tpml_pcr_selection_get_tpms_pcr_selection(
                TPML_PCR_SELECTION *l,
                TPMI_ALG_HASH hash_alg) {

        assert(l);
        assert(l->count <= ELEMENTSOF(l->pcrSelections));

        TPMS_PCR_SELECTION *selection = NULL;
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(s, l)
                if (s->hash == hash_alg) {
                        selection = s;
                        break;
                }

        if (!selection)
                return NULL;

        /* Iterate backwards through the entries, removing any other entries for the hash alg. */
        for (uint32_t i = l->count - 1; i > 0; i--) {
                TPMS_PCR_SELECTION *s = &l->pcrSelections[i];

                if (selection == s)
                        break;

                if (s->hash == hash_alg) {
                        tpm2_tpms_pcr_selection_move(selection, s);
                        tpm2_tpml_pcr_selection_remove_index(l, i);
                }
        }

        return selection;
}

/* Convert a TPML_PCR_SELECTION object to a mask. Returns -ENOENT if 'hash_alg' is not in the object. */
int tpm2_tpml_pcr_selection_to_mask(const TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash_alg, uint32_t *ret) {
        assert(l);
        assert(ret);

        /* Make a copy, as tpm2_tpml_pcr_selection_get_tpms_pcr_selection() will modify the object if there
         * are multiple entries with the requested hash alg. */
        TPML_PCR_SELECTION lcopy = *l;

        TPMS_PCR_SELECTION *s;
        s = tpm2_tpml_pcr_selection_get_tpms_pcr_selection(&lcopy, hash_alg);
        if (!s)
                return SYNTHETIC_ERRNO(ENOENT);

        tpm2_tpms_pcr_selection_to_mask(s, ret);
        return 0;
}

/* Convert a mask and hash alg to a TPML_PCR_SELECTION object. */
void tpm2_tpml_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash_alg, TPML_PCR_SELECTION *ret) {
        assert(ret);

        TPMS_PCR_SELECTION s;
        tpm2_tpms_pcr_selection_from_mask(mask, hash_alg, &s);

        *ret = (TPML_PCR_SELECTION){
                .count = 1,
                .pcrSelections[0] = s,
        };
}

/* Combine all duplicate (same hash alg) TPMS_PCR_SELECTION entries in 'l'. */
static void tpm2_tpml_pcr_selection_cleanup(TPML_PCR_SELECTION *l) {
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(s, l)
                /* This removes all duplicates for s->hash. */
                (void) tpm2_tpml_pcr_selection_get_tpms_pcr_selection(l, s->hash);
}

/* Add the PCR selections in 's' to the corresponding hash alg TPMS_PCR_SELECTION entry in 'l'. Adds a new
 * TPMS_PCR_SELECTION entry for the hash alg if needed. This may modify the TPML_PCR_SELECTION by combining
 * entries with the same hash alg. */
void tpm2_tpml_pcr_selection_add_tpms_pcr_selection(TPML_PCR_SELECTION *l, const TPMS_PCR_SELECTION *s) {
        assert(l);
        assert(s);

        if (tpm2_tpms_pcr_selection_is_empty(s))
                return;

        TPMS_PCR_SELECTION *selection = tpm2_tpml_pcr_selection_get_tpms_pcr_selection(l, s->hash);
        if (selection) {
                tpm2_tpms_pcr_selection_add(selection, s);
                return;
        }

        /* It's already broken if the count is higher than the array has size for. */
        assert(l->count <= ELEMENTSOF(l->pcrSelections));

        /* If full, the cleanup should result in at least one available entry. */
        if (l->count == ELEMENTSOF(l->pcrSelections))
                tpm2_tpml_pcr_selection_cleanup(l);

        assert(l->count < ELEMENTSOF(l->pcrSelections));
        l->pcrSelections[l->count++] = *s;
}

/* Remove the PCR selections in 's' from the corresponding hash alg TPMS_PCR_SELECTION entry in 'l'. This
 * will combine all entries for 's->hash' in 'l'. */
void tpm2_tpml_pcr_selection_sub_tpms_pcr_selection(TPML_PCR_SELECTION *l, const TPMS_PCR_SELECTION *s) {
        assert(l);
        assert(s);

        if (tpm2_tpms_pcr_selection_is_empty(s))
                return;

        TPMS_PCR_SELECTION *selection = tpm2_tpml_pcr_selection_get_tpms_pcr_selection(l, s->hash);
        if (selection)
                tpm2_tpms_pcr_selection_sub(selection, s);
}

/* Add all PCR selections in 'b' to 'a'. */
void tpm2_tpml_pcr_selection_add(TPML_PCR_SELECTION *a, const TPML_PCR_SELECTION *b) {
        assert(a);
        assert(b);

        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(selection_b, (TPML_PCR_SELECTION*) b)
                tpm2_tpml_pcr_selection_add_tpms_pcr_selection(a, selection_b);
}

/* Remove all PCR selections in 'b' from 'a'. */
void tpm2_tpml_pcr_selection_sub(TPML_PCR_SELECTION *a, const TPML_PCR_SELECTION *b) {
        assert(a);
        assert(b);

        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(selection_b, (TPML_PCR_SELECTION*) b)
                tpm2_tpml_pcr_selection_sub_tpms_pcr_selection(a, selection_b);
}

char *tpm2_tpml_pcr_selection_to_string(const TPML_PCR_SELECTION *l) {
        assert(l);

        _cleanup_free_ char *banks = NULL;
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(s, (TPML_PCR_SELECTION*) l) {
                if (tpm2_tpms_pcr_selection_is_empty(s))
                        continue;

                _cleanup_free_ char *str = tpm2_tpms_pcr_selection_to_string(s);
                if (!str || !strextend_with_separator(&banks, ",", str))
                        return NULL;
        }

        return strjoin("[", strempty(banks), "]");
}

size_t tpm2_tpml_pcr_selection_weight(const TPML_PCR_SELECTION *l) {
        assert(l);
        assert(l->count <= ELEMENTSOF(l->pcrSelections));

        size_t weight = 0;
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(s, l) {
                size_t w = tpm2_tpms_pcr_selection_weight(s);
                assert(weight <= SIZE_MAX - w);
                weight += w;
        }

        return weight;
}

static void tpm2_log_debug_tpml_pcr_selection(const TPML_PCR_SELECTION *l, const char *msg) {
        if (!DEBUG_LOGGING || !l)
                return;

        _cleanup_free_ char *s = tpm2_tpml_pcr_selection_to_string(l);
        log_debug("%s: %s", msg ?: "PCR selection", strna(s));
}

static void tpm2_log_debug_buffer(const void *buffer, size_t size, const char *msg) {
        if (!DEBUG_LOGGING || !buffer || size == 0)
                return;

        _cleanup_free_ char *h = hexmem(buffer, size);
        log_debug("%s: %s", msg ?: "Buffer", strna(h));
}

static void tpm2_log_debug_digest(const TPM2B_DIGEST *digest, const char *msg) {
        if (digest)
                tpm2_log_debug_buffer(digest->buffer, digest->size, msg ?: "Digest");
}

static void tpm2_log_debug_name(const TPM2B_NAME *name, const char *msg) {
        if (name)
                tpm2_log_debug_buffer(name->name, name->size, msg ?: "Name");
}

static int tpm2_get_policy_digest(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2B_DIGEST **ret_policy_digest) {

        TSS2_RC rc;

        if (!DEBUG_LOGGING && !ret_policy_digest)
                return 0;

        assert(c);
        assert(session);

        log_debug("Acquiring policy digest.");

        _cleanup_(Esys_Freep) TPM2B_DIGEST *policy_digest = NULL;
        rc = sym_Esys_PolicyGetDigest(
                        c->esys_context,
                        session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &policy_digest);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to get policy digest from TPM: %s", sym_Tss2_RC_Decode(rc));

        tpm2_log_debug_digest(policy_digest, "Session policy digest");

        if (ret_policy_digest)
                *ret_policy_digest = TAKE_PTR(policy_digest);

        return 0;
}

int tpm2_create_primary(
                Tpm2Context *c,
                const Tpm2Handle *session,
                const TPM2B_PUBLIC *template,
                const TPM2B_SENSITIVE_CREATE *sensitive,
                TPM2B_PUBLIC **ret_public,
                Tpm2Handle **ret_handle) {

        usec_t ts;
        TSS2_RC rc;
        int r;

        assert(c);
        assert(template);

        log_debug("Creating primary key on TPM.");

        ts = now(CLOCK_MONOTONIC);

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        rc = sym_Esys_CreatePrimary(
                        c->esys_context,
                        ESYS_TR_RH_OWNER,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        sensitive ? sensitive : &(TPM2B_SENSITIVE_CREATE) {},
                        template,
                        /* outsideInfo= */ NULL,
                        &(TPML_PCR_SELECTION) {},
                        &handle->esys_handle,
                        &public,
                        /* creationData= */ NULL,
                        /* creationHash= */ NULL,
                        /* creationTicket= */ NULL);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to generate primary key in TPM: %s",
                                       sym_Tss2_RC_Decode(rc));

        log_debug("Successfully created primary key on TPM in %s.",
                  FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - ts, USEC_PER_MSEC));

        if (ret_public)
                *ret_public = TAKE_PTR(public);
        if (ret_handle)
                *ret_handle = TAKE_PTR(handle);

        return 0;
}

/* Create a TPM object. Do not use this to create primary keys, because some HW TPMs refuse to allow that;
 * instead use tpm2_create_primary(). */
int tpm2_create(Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPMT_PUBLIC *template,
                const TPMS_SENSITIVE_CREATE *sensitive,
                TPM2B_PUBLIC **ret_public,
                TPM2B_PRIVATE **ret_private) {

        usec_t ts;
        TSS2_RC rc;

        assert(c);
        assert(parent);
        assert(template);

        log_debug("Creating object on TPM.");

        ts = now(CLOCK_MONOTONIC);

        TPM2B_PUBLIC tpm2b_public = {
                .size = sizeof(*template) - sizeof(template->unique),
                .publicArea = *template,
        };

        /* Zero the unique area. */
        zero(tpm2b_public.publicArea.unique);

        TPM2B_SENSITIVE_CREATE tpm2b_sensitive;
        if (sensitive)
                tpm2b_sensitive = (TPM2B_SENSITIVE_CREATE) {
                        .size = sizeof(*sensitive),
                        .sensitive = *sensitive,
                };
        else
                tpm2b_sensitive = (TPM2B_SENSITIVE_CREATE) {};

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        _cleanup_(Esys_Freep) TPM2B_PRIVATE *private = NULL;
        rc = sym_Esys_Create(
                        c->esys_context,
                        parent->esys_handle,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &tpm2b_sensitive,
                        &tpm2b_public,
                        /* outsideInfo= */ NULL,
                        &(TPML_PCR_SELECTION) {},
                        &private,
                        &public,
                        /* creationData= */ NULL,
                        /* creationHash= */ NULL,
                        /* creationTicket= */ NULL);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to generate object in TPM: %s",
                                       sym_Tss2_RC_Decode(rc));

        log_debug("Successfully created object on TPM in %s.",
                  FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - ts, USEC_PER_MSEC));

        if (ret_public)
                *ret_public = TAKE_PTR(public);
        if (ret_private)
                *ret_private = TAKE_PTR(private);

        return 0;
}

static int tpm2_load(
                Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPM2B_PUBLIC *public,
                const TPM2B_PRIVATE *private,
                Tpm2Handle **ret_handle) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(public);
        assert(private);
        assert(ret_handle);

        log_debug("Loading object into TPM.");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        rc = sym_Esys_Load(
                        c->esys_context,
                        parent ? parent->esys_handle : ESYS_TR_RH_OWNER,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        private,
                        public,
                        &handle->esys_handle);
        if (rc == TPM2_RC_LOCKOUT)
                return log_error_errno(SYNTHETIC_ERRNO(ENOLCK),
                                       "TPM2 device is in dictionary attack lockout mode.");
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to load key into TPM: %s", sym_Tss2_RC_Decode(rc));

        *ret_handle = TAKE_PTR(handle);

        return 0;
}

static int tpm2_load_external(
                Tpm2Context *c,
                const Tpm2Handle *session,
                const TPM2B_PUBLIC *public,
                const TPM2B_SENSITIVE *private,
                Tpm2Handle **ret_handle) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(ret_handle);

        log_debug("Loading external key into TPM.");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        rc = sym_Esys_LoadExternal(
                        c->esys_context,
                        session ? session->esys_handle : ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        private,
                        public,
#if HAVE_TSS2_ESYS3
                        /* tpm2-tss >= 3.0.0 requires a ESYS_TR_RH_* constant specifying the requested
                         * hierarchy, older versions need TPM2_RH_* instead. */
                        ESYS_TR_RH_OWNER,
#else
                        TPM2_RH_OWNER,
#endif
                        &handle->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to load public key into TPM: %s", sym_Tss2_RC_Decode(rc));

        *ret_handle = TAKE_PTR(handle);

        return 0;
}

/* This calls TPM2_CreateLoaded() directly, without checking if the TPM supports it. Callers should instead
 * use tpm2_create_loaded(). */
static int _tpm2_create_loaded(
                Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPMT_PUBLIC *template,
                const TPMS_SENSITIVE_CREATE *sensitive,
                TPM2B_PUBLIC **ret_public,
                TPM2B_PRIVATE **ret_private,
                Tpm2Handle **ret_handle) {

        usec_t ts;
        TSS2_RC rc;
        int r;

        assert(c);
        assert(parent);
        assert(template);

        log_debug("Creating loaded object on TPM.");

        ts = now(CLOCK_MONOTONIC);

        /* Copy the input template and zero the unique area. */
        TPMT_PUBLIC template_copy = *template;
        zero(template_copy.unique);

        TPM2B_TEMPLATE tpm2b_template;
        size_t size = 0;
        rc = sym_Tss2_MU_TPMT_PUBLIC_Marshal(
                        &template_copy,
                        tpm2b_template.buffer,
                        sizeof(tpm2b_template.buffer),
                        &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal public key template: %s", sym_Tss2_RC_Decode(rc));
        assert(size <= UINT16_MAX);
        tpm2b_template.size = size;

        TPM2B_SENSITIVE_CREATE tpm2b_sensitive;
        if (sensitive)
                tpm2b_sensitive = (TPM2B_SENSITIVE_CREATE) {
                        .size = sizeof(*sensitive),
                        .sensitive = *sensitive,
                };
        else
                tpm2b_sensitive = (TPM2B_SENSITIVE_CREATE) {};

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        _cleanup_(Esys_Freep) TPM2B_PRIVATE *private = NULL;
        rc = sym_Esys_CreateLoaded(
                        c->esys_context,
                        parent->esys_handle,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &tpm2b_sensitive,
                        &tpm2b_template,
                        &handle->esys_handle,
                        &private,
                        &public);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to generate loaded object in TPM: %s",
                                       sym_Tss2_RC_Decode(rc));

        log_debug("Successfully created loaded object on TPM in %s.",
                  FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - ts, USEC_PER_MSEC));

        if (ret_public)
                *ret_public = TAKE_PTR(public);
        if (ret_private)
                *ret_private = TAKE_PTR(private);
        if (ret_handle)
                *ret_handle = TAKE_PTR(handle);

        return 0;
}

/* This calls TPM2_CreateLoaded() if the TPM supports it, otherwise it calls TPM2_Create() and TPM2_Load()
 * separately. Do not use this to create primary keys, because some HW TPMs refuse to allow that; instead use
 * tpm2_create_primary(). */
int tpm2_create_loaded(
                Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPMT_PUBLIC *template,
                const TPMS_SENSITIVE_CREATE *sensitive,
                TPM2B_PUBLIC **ret_public,
                TPM2B_PRIVATE **ret_private,
                Tpm2Handle **ret_handle) {

        int r;

        if (tpm2_supports_command(c, TPM2_CC_CreateLoaded))
                return _tpm2_create_loaded(c, parent, session, template, sensitive, ret_public, ret_private, ret_handle);

        /* Unfortunately, this TPM doesn't support CreateLoaded (added at spec revision 130) so we need to
         * create and load manually. */
        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        _cleanup_(Esys_Freep) TPM2B_PRIVATE *private = NULL;
        r = tpm2_create(c, parent, session, template, sensitive, &public, &private);
        if (r < 0)
                return r;

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_load(c, parent, session, public, private, &handle);
        if (r < 0)
                return r;

        if (ret_public)
                *ret_public = TAKE_PTR(public);
        if (ret_private)
                *ret_private = TAKE_PTR(private);
        if (ret_handle)
                *ret_handle = TAKE_PTR(handle);

        return 0;
}

static int tpm2_pcr_read(
                Tpm2Context *c,
                const TPML_PCR_SELECTION *pcr_selection,
                TPML_PCR_SELECTION *ret_pcr_selection,
                TPM2B_DIGEST **ret_pcr_values,
                size_t *ret_n_pcr_values) {

        _cleanup_free_ TPM2B_DIGEST *pcr_values = NULL;
        TPML_PCR_SELECTION remaining, total_read = {};
        size_t n_pcr_values = 0;
        TSS2_RC rc;

        assert(c);
        assert(pcr_selection);

        remaining = *pcr_selection;
        while (!tpm2_tpml_pcr_selection_is_empty(&remaining)) {
                _cleanup_(Esys_Freep) TPML_PCR_SELECTION *current_read = NULL;
                _cleanup_(Esys_Freep) TPML_DIGEST *current_values = NULL;

                tpm2_log_debug_tpml_pcr_selection(&remaining, "Reading PCR selection");

                /* Unfortunately, PCR_Read will not return more than 8 values. */
                rc = sym_Esys_PCR_Read(
                                c->esys_context,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                &remaining,
                                NULL,
                                &current_read,
                                &current_values);
                if (rc != TSS2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to read TPM2 PCRs: %s", sym_Tss2_RC_Decode(rc));

                if (tpm2_tpml_pcr_selection_is_empty(current_read)) {
                        log_warning("TPM2 refused to read possibly unimplemented PCRs, ignoring.");
                        break;
                }

                tpm2_tpml_pcr_selection_sub(&remaining, current_read);
                tpm2_tpml_pcr_selection_add(&total_read, current_read);

                if (!GREEDY_REALLOC(pcr_values, n_pcr_values + current_values->count))
                        return log_oom();

                memcpy_safe(&pcr_values[n_pcr_values], current_values->digests,
                            current_values->count * sizeof(TPM2B_DIGEST));
                n_pcr_values += current_values->count;

                if (DEBUG_LOGGING) {
                        unsigned i = 0;
                        FOREACH_PCR_IN_TPML_PCR_SELECTION(pcr, s, current_read) {
                                assert(i < current_values->count);

                                TPM2B_DIGEST *d = &current_values->digests[i];
                                i++;

                                TPML_PCR_SELECTION l;
                                tpm2_tpml_pcr_selection_from_mask(INDEX_TO_MASK(uint32_t, pcr), s->hash, &l);

                                _cleanup_free_ char *desc = tpm2_tpml_pcr_selection_to_string(&l);
                                tpm2_log_debug_digest(d, strna(desc));
                        }
                }
        }

        if (ret_pcr_selection)
                *ret_pcr_selection = total_read;
        if (ret_pcr_values)
                *ret_pcr_values = TAKE_PTR(pcr_values);
        if (ret_n_pcr_values)
                *ret_n_pcr_values = n_pcr_values;

        return 0;
}

static int tpm2_pcr_mask_good(
                Tpm2Context *c,
                TPMI_ALG_HASH bank,
                uint32_t mask) {

        _cleanup_free_ TPM2B_DIGEST *pcr_values = NULL;
        TPML_PCR_SELECTION selection;
        size_t n_pcr_values = 0;
        int r;

        assert(c);

        /* So we have the problem that some systems might have working TPM2 chips, but the firmware doesn't
         * actually measure into them, or only into a suboptimal bank. If so, the PCRs should be all zero or
         * all 0xFF. Detect that, so that we can warn and maybe pick a better bank. */

        tpm2_tpml_pcr_selection_from_mask(mask, bank, &selection);

        r = tpm2_pcr_read(c, &selection, &selection, &pcr_values, &n_pcr_values);
        if (r < 0)
                return r;

        /* If at least one of the selected PCR values is something other than all 0x00 or all 0xFF we are happy. */
        unsigned i = 0;
        FOREACH_PCR_IN_TPML_PCR_SELECTION(pcr, s, &selection) {
                assert(i < n_pcr_values);

                if (!memeqbyte(0x00, pcr_values[i].buffer, pcr_values[i].size) &&
                    !memeqbyte(0xFF, pcr_values[i].buffer, pcr_values[i].size))
                        return true;

                i++;
        }

        return false;
}

static int tpm2_bank_has24(const TPMS_PCR_SELECTION *selection) {

        assert(selection);

        /* As per https://trustedcomputinggroup.org/wp-content/uploads/TCG_PCClient_PFP_r1p05_v23_pub.pdf a
         * TPM2 on a Client PC must have at least 24 PCRs. If this TPM has less, just skip over it. */
        if (selection->sizeofSelect < TPM2_PCRS_MAX/8) {
                log_debug("Skipping TPM2 PCR bank %s with fewer than 24 PCRs.",
                          strna(tpm2_hash_alg_to_string(selection->hash)));
                return false;
        }

        assert_cc(TPM2_PCRS_MAX % 8 == 0);

        /* It's not enough to check how many PCRs there are, we also need to check that the 24 are
         * enabled for this bank. Otherwise this TPM doesn't qualify. */
        bool valid = true;
        for (size_t j = 0; j < TPM2_PCRS_MAX/8; j++)
                if (selection->pcrSelect[j] != 0xFF) {
                        valid = false;
                        break;
                }

        if (!valid)
                log_debug("TPM2 PCR bank %s has fewer than 24 PCR bits enabled, ignoring.",
                          strna(tpm2_hash_alg_to_string(selection->hash)));

        return valid;
}

static int tpm2_get_best_pcr_bank(
                Tpm2Context *c,
                uint32_t pcr_mask,
                TPMI_ALG_HASH *ret) {

        TPMI_ALG_HASH supported_hash = 0, hash_with_valid_pcr = 0;
        int r;

        assert(c);
        assert(ret);

        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(selection, &c->capability_pcrs) {
                TPMI_ALG_HASH hash = selection->hash;
                int good;

                /* For now we are only interested in the SHA1 and SHA256 banks */
                if (!IN_SET(hash, TPM2_ALG_SHA256, TPM2_ALG_SHA1))
                        continue;

                r = tpm2_bank_has24(selection);
                if (r < 0)
                        return r;
                if (!r)
                        continue;

                good = tpm2_pcr_mask_good(c, hash, pcr_mask);
                if (good < 0)
                        return good;

                if (hash == TPM2_ALG_SHA256) {
                        supported_hash = TPM2_ALG_SHA256;
                        if (good) {
                                /* Great, SHA256 is supported and has initialized PCR values, we are done. */
                                hash_with_valid_pcr = TPM2_ALG_SHA256;
                                break;
                        }
                } else {
                        assert(hash == TPM2_ALG_SHA1);

                        if (supported_hash == 0)
                                supported_hash = TPM2_ALG_SHA1;

                        if (good && hash_with_valid_pcr == 0)
                                hash_with_valid_pcr = TPM2_ALG_SHA1;
                }
        }

        /* We preferably pick SHA256, but only if its PCRs are initialized or neither the SHA1 nor the SHA256
         * PCRs are initialized. If SHA256 is not supported but SHA1 is and its PCRs are too, we prefer
         * SHA1.
         *
         * We log at LOG_NOTICE level whenever we end up using the SHA1 bank or when the PCRs we bind to are
         * not initialized. */

        if (hash_with_valid_pcr == TPM2_ALG_SHA256) {
                assert(supported_hash == TPM2_ALG_SHA256);
                log_debug("TPM2 device supports SHA256 PCR bank and SHA256 PCRs are valid, yay!");
                *ret = TPM2_ALG_SHA256;
        } else if (hash_with_valid_pcr == TPM2_ALG_SHA1) {
                if (supported_hash == TPM2_ALG_SHA256)
                        log_notice("TPM2 device supports both SHA1 and SHA256 PCR banks, but only SHA1 PCRs are valid, falling back to SHA1 bank. This reduces the security level substantially.");
                else {
                        assert(supported_hash == TPM2_ALG_SHA1);
                        log_notice("TPM2 device lacks support for SHA256 PCR bank, but SHA1 bank is supported and SHA1 PCRs are valid, falling back to SHA1 bank. This reduces the security level substantially.");
                }

                *ret = TPM2_ALG_SHA1;
        } else if (supported_hash == TPM2_ALG_SHA256) {
                log_notice("TPM2 device supports SHA256 PCR bank but none of the selected PCRs are valid! Firmware apparently did not initialize any of the selected PCRs. Proceeding anyway with SHA256 bank. PCR policy effectively unenforced!");
                *ret = TPM2_ALG_SHA256;
        } else if (supported_hash == TPM2_ALG_SHA1) {
                log_notice("TPM2 device lacks support for SHA256 bank, but SHA1 bank is supported, but none of the selected PCRs are valid! Firmware apparently did not initialize any of the selected PCRs. Proceeding anyway with SHA1 bank. PCR policy effectively unenforced!");
                *ret = TPM2_ALG_SHA1;
        } else
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "TPM2 module supports neither SHA1 nor SHA256 PCR banks, cannot operate.");

        return 0;
}

int tpm2_get_good_pcr_banks(
                Tpm2Context *c,
                uint32_t pcr_mask,
                TPMI_ALG_HASH **ret) {

        _cleanup_free_ TPMI_ALG_HASH *good_banks = NULL, *fallback_banks = NULL;
        size_t n_good_banks = 0, n_fallback_banks = 0;
        int r;

        assert(c);
        assert(ret);

        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(selection, &c->capability_pcrs) {
                TPMI_ALG_HASH hash = selection->hash;

                /* Let's see if this bank is superficially OK, i.e. has at least 24 enabled registers */
                r = tpm2_bank_has24(selection);
                if (r < 0)
                        return r;
                if (!r)
                        continue;

                /* Let's now see if this bank has any of the selected PCRs actually initialized */
                r = tpm2_pcr_mask_good(c, hash, pcr_mask);
                if (r < 0)
                        return r;

                if (n_good_banks + n_fallback_banks >= INT_MAX)
                        return log_error_errno(SYNTHETIC_ERRNO(E2BIG), "Too many good TPM2 banks?");

                if (r) {
                        if (!GREEDY_REALLOC(good_banks, n_good_banks+1))
                                return log_oom();

                        good_banks[n_good_banks++] = hash;
                } else {
                        if (!GREEDY_REALLOC(fallback_banks, n_fallback_banks+1))
                                return log_oom();

                        fallback_banks[n_fallback_banks++] = hash;
                }
        }

        /* Preferably, use the good banks (i.e. the ones the PCR values are actually initialized so
         * far). Otherwise use the fallback banks (i.e. which exist and are enabled, but so far not used. */
        if (n_good_banks > 0) {
                log_debug("Found %zu fully initialized TPM2 banks.", n_good_banks);
                *ret = TAKE_PTR(good_banks);
                return (int) n_good_banks;
        }
        if (n_fallback_banks > 0) {
                log_debug("Found %zu enabled but un-initialized TPM2 banks.", n_fallback_banks);
                *ret = TAKE_PTR(fallback_banks);
                return (int) n_fallback_banks;
        }

        /* No suitable banks found. */
        *ret = NULL;
        return 0;
}

int tpm2_get_good_pcr_banks_strv(
                Tpm2Context *c,
                uint32_t pcr_mask,
                char ***ret) {

#if HAVE_OPENSSL
        _cleanup_free_ TPMI_ALG_HASH *algs = NULL;
        _cleanup_strv_free_ char **l = NULL;
        int n_algs;

        assert(c);
        assert(ret);

        n_algs = tpm2_get_good_pcr_banks(c, pcr_mask, &algs);
        if (n_algs < 0)
                return n_algs;

        for (int i = 0; i < n_algs; i++) {
                _cleanup_free_ char *n = NULL;
                const EVP_MD *implementation;
                const char *salg;

                salg = tpm2_hash_alg_to_string(algs[i]);
                if (!salg)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM2 operates with unknown PCR algorithm, can't measure.");

                implementation = EVP_get_digestbyname(salg);
                if (!implementation)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM2 operates with unsupported PCR algorithm, can't measure.");

                n = strdup(ASSERT_PTR(EVP_MD_name(implementation)));
                if (!n)
                        return log_oom();

                ascii_strlower(n); /* OpenSSL uses uppercase digest names, we prefer them lower case. */

                if (strv_consume(&l, TAKE_PTR(n)) < 0)
                        return log_oom();
        }

        *ret = TAKE_PTR(l);
        return 0;
#else /* HAVE_OPENSSL */
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}

/* Hash data into the digest.
 *
 * If 'extend' is true, the hashing operation starts with the existing digest hash (and the digest is
 * required to have a hash and its size must be correct). If 'extend' is false, the digest size is
 * initialized to the correct size for 'alg' and the hashing operation does not include any existing digest
 * hash. If 'extend' is false and no data is provided, the digest is initialized to a zero digest.
 *
 * On success, the digest hash will be updated with the hashing operation result and the digest size will be
 * correct for 'alg'.
 *
 * This currently only provides SHA256, so 'alg' must be TPM2_ALG_SHA256. */
int tpm2_digest_many(
                TPMI_ALG_HASH alg,
                TPM2B_DIGEST *digest,
                const struct iovec data[],
                size_t n_data,
                bool extend) {

        struct sha256_ctx ctx;

        assert(digest);
        assert(data || n_data == 0);

        if (alg != TPM2_ALG_SHA256)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Hash algorithm not supported: 0x%x", alg);

        if (extend && digest->size != SHA256_DIGEST_SIZE)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Digest size 0x%x, require 0x%x",
                                       digest->size, (unsigned)SHA256_DIGEST_SIZE);

        /* Since we're hardcoding SHA256 (for now), we can check this at compile time. */
        assert_cc(sizeof(digest->buffer) >= SHA256_DIGEST_SIZE);

        CLEANUP_ERASE(ctx);

        sha256_init_ctx(&ctx);

        if (extend)
                sha256_process_bytes(digest->buffer, digest->size, &ctx);
        else {
                *digest = (TPM2B_DIGEST){ .size = SHA256_DIGEST_SIZE, };
                if (n_data == 0) /* If not extending and no data, return zero hash */
                        return 0;
        }

        for (size_t i = 0; i < n_data; i++)
                sha256_process_bytes(data[i].iov_base, data[i].iov_len, &ctx);

        sha256_finish_ctx(&ctx, digest->buffer);

        return 0;
}

/* Same as tpm2_digest_many() but data is contained in TPM2B_DIGEST[]. The digests may be any size digests. */
int tpm2_digest_many_digests(
                TPMI_ALG_HASH alg,
                TPM2B_DIGEST *digest,
                const TPM2B_DIGEST data[],
                size_t n_data,
                bool extend) {

        _cleanup_free_ struct iovec *iovecs = NULL;

        assert(data || n_data == 0);

        iovecs = new(struct iovec, n_data);
        if (!iovecs)
                return log_oom();

        for (size_t i = 0; i < n_data; i++)
                iovecs[i] = IOVEC_MAKE((void*) data[i].buffer, data[i].size);

        return tpm2_digest_many(alg, digest, iovecs, n_data, extend);
}

/* This hashes the provided pin into a digest value, but also verifies that the final byte is not 0, because
 * the TPM specification Part 1 ("Architecture") section Authorization Values (subsection "Authorization Size
 * Convention") states "Trailing octets of zero are to be removed from any string before it is used as an
 * authValue". Since the TPM doesn't know if the auth value is a "string" or just a hash digest, any hash
 * digest that randomly happens to end in 0 must have the final 0(s) trimmed.
 *
 * This is required at 2 points. First, when setting the authValue during creation of new sealed objects, in
 * tpm2_seal(). This only applies to newly created objects, of course.  Second, when using a previously
 * created sealed object that has an authValue set, we use the sealed objects as the session bind key. This
 * requires calling SetAuth so tpm2-tss can correctly calculate the HMAC to use for the encryption session.
 *
 * TPM implementations will perform the trimming for any authValue for existing sealed objects, so the
 * tpm2-tss library must also perform the trimming before HMAC calculation, but it does not yet; this bug is
 * open to add the trimming: https://github.com/tpm2-software/tpm2-tss/issues/2664
 *
 * Until our minimum tpm2-tss version contains a fix for that bug, we must perform the trimming
 * ourselves. Note that since we are trimming, which is exactly what a TPM implementation would do, this will
 * work for both existing objects with a authValue ending in 0(s) as well as new sealed objects we create,
 * which we will trim the 0(s) from before sending to the TPM.
 */
static void tpm2_trim_auth_value(TPM2B_AUTH *auth) {
        bool trimmed = false;

        assert(auth);

        while (auth->size > 0 && auth->buffer[auth->size - 1] == 0) {
                trimmed = true;
                auth->size--;
        }

        if (trimmed)
                log_debug("authValue ends in 0, trimming as required by the TPM2 specification Part 1 section 'HMAC Computation' authValue Note 2.");
}

static int tpm2_get_pin_auth(TPMI_ALG_HASH hash, const char *pin, TPM2B_AUTH *ret_auth) {
        TPM2B_AUTH auth = {};
        int r;

        assert(pin);
        assert(ret_auth);

        r = tpm2_digest_buffer(hash, &auth, pin, strlen(pin), /* extend= */ false);
        if (r < 0)
                return r;

        tpm2_trim_auth_value(&auth);

        *ret_auth = TAKE_STRUCT(auth);

        return 0;
}

static int tpm2_set_auth(Tpm2Context *c, const Tpm2Handle *handle, const char *pin) {
        TPM2B_AUTH auth = {};
        TSS2_RC rc;
        int r;

        assert(c);
        assert(handle);

        if (!pin)
                return 0;

        CLEANUP_ERASE(auth);

        r = tpm2_get_pin_auth(TPM2_ALG_SHA256, pin, &auth);
        if (r < 0)
                return r;

        rc = sym_Esys_TR_SetAuth(c->esys_context, handle->esys_handle, &auth);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to load PIN in TPM: %s", sym_Tss2_RC_Decode(rc));

        return 0;
}

static bool tpm2_is_encryption_session(Tpm2Context *c, const Tpm2Handle *session) {
        TPMA_SESSION flags = 0;
        TSS2_RC rc;

        assert(c);
        assert(session);

        rc = sym_Esys_TRSess_GetAttributes(c->esys_context, session->esys_handle, &flags);
        if (rc != TSS2_RC_SUCCESS)
                return false;

        return (flags & TPMA_SESSION_DECRYPT) && (flags & TPMA_SESSION_ENCRYPT);
}

static int tpm2_make_encryption_session(
                Tpm2Context *c,
                const Tpm2Handle *primary,
                const Tpm2Handle *bind_key,
                Tpm2Handle **ret_session) {

        const TPMA_SESSION sessionAttributes = TPMA_SESSION_DECRYPT | TPMA_SESSION_ENCRYPT |
                        TPMA_SESSION_CONTINUESESSION;
        TSS2_RC rc;
        int r;

        assert(c);
        assert(ret_session);

        log_debug("Starting HMAC encryption session.");

        /* Start a salted, unbound HMAC session with a well-known key (e.g. primary key) as tpmKey, which
         * means that the random salt will be encrypted with the well-known key. That way, only the TPM can
         * recover the salt, which is then used for key derivation. */
        _cleanup_(tpm2_handle_freep) Tpm2Handle *session = NULL;
        r = tpm2_handle_new(c, &session);
        if (r < 0)
                return r;

        rc = sym_Esys_StartAuthSession(
                        c->esys_context,
                        primary->esys_handle,
                        bind_key->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        NULL,
                        TPM2_SE_HMAC,
                        &SESSION_TEMPLATE_SYM_AES_128_CFB,
                        TPM2_ALG_SHA256,
                        &session->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to open session in TPM: %s", sym_Tss2_RC_Decode(rc));

        /* Enable parameter encryption/decryption with AES in CFB mode. Together with HMAC digests (which are
         * always used for sessions), this provides confidentiality, integrity and replay protection for
         * operations that use this session. */
        rc = sym_Esys_TRSess_SetAttributes(c->esys_context, session->esys_handle, sessionAttributes, 0xff);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(
                                SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                "Failed to configure TPM session: %s",
                                sym_Tss2_RC_Decode(rc));

        *ret_session = TAKE_PTR(session);

        return 0;
}

static int tpm2_make_policy_session(
                Tpm2Context *c,
                const Tpm2Handle *primary,
                const Tpm2Handle *encryption_session,
                bool trial,
                Tpm2Handle **ret_session) {

        TPM2_SE session_type = trial ? TPM2_SE_TRIAL : TPM2_SE_POLICY;
        TSS2_RC rc;
        int r;

        assert(c);
        assert(primary);
        assert(encryption_session);
        assert(ret_session);

        if (!tpm2_is_encryption_session(c, encryption_session))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Missing encryption session");

        log_debug("Starting policy session.");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *session = NULL;
        r = tpm2_handle_new(c, &session);
        if (r < 0)
                return r;

        rc = sym_Esys_StartAuthSession(
                        c->esys_context,
                        primary->esys_handle,
                        ESYS_TR_NONE,
                        encryption_session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        NULL,
                        session_type,
                        &SESSION_TEMPLATE_SYM_AES_128_CFB,
                        TPM2_ALG_SHA256,
                        &session->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to open session in TPM: %s", sym_Tss2_RC_Decode(rc));

        *ret_session = TAKE_PTR(session);

        return 0;
}

static int openssl_pubkey_to_tpm2_pubkey(
                const void *pubkey,
                size_t pubkey_size,
                TPM2B_PUBLIC *output,
                void **ret_fp,
                size_t *ret_fp_size) {

#if HAVE_OPENSSL
#if OPENSSL_VERSION_MAJOR >= 3
        _cleanup_(BN_freep) BIGNUM *n = NULL, *e = NULL;
#else
        const BIGNUM *n = NULL, *e = NULL;
        const RSA *rsa = NULL;
#endif
        int r, n_bytes, e_bytes;

        assert(pubkey);
        assert(pubkey_size > 0);
        assert(output);

        /* Converts an OpenSSL public key to a structure that the TPM chip can process. */

        _cleanup_fclose_ FILE *f = NULL;
        f = fmemopen((void*) pubkey, pubkey_size, "r");
        if (!f)
                return log_oom();

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *input = NULL;
        input = PEM_read_PUBKEY(f, NULL, NULL, NULL);
        if (!input)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to parse PEM public key.");

        if (EVP_PKEY_base_id(input) != EVP_PKEY_RSA)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Provided public key is not an RSA key.");

#if OPENSSL_VERSION_MAJOR >= 3
        if (!EVP_PKEY_get_bn_param(input, OSSL_PKEY_PARAM_RSA_N, &n))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to get RSA modulus from public key.");
#else
        rsa = EVP_PKEY_get0_RSA(input);
        if (!rsa)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to extract RSA key from public key.");

        n = RSA_get0_n(rsa);
        if (!n)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to get RSA modulus from public key.");
#endif

        n_bytes = BN_num_bytes(n);
        assert_se(n_bytes > 0);
        if ((size_t) n_bytes > sizeof_field(TPM2B_PUBLIC, publicArea.unique.rsa.buffer))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "RSA modulus too large for TPM2 public key object.");

#if OPENSSL_VERSION_MAJOR >= 3
        if (!EVP_PKEY_get_bn_param(input, OSSL_PKEY_PARAM_RSA_E, &e))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to get RSA exponent from public key.");
#else
        e = RSA_get0_e(rsa);
        if (!e)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to get RSA exponent from public key.");
#endif

        e_bytes = BN_num_bytes(e);
        assert_se(e_bytes > 0);
        if ((size_t) e_bytes > sizeof_field(TPM2B_PUBLIC, publicArea.parameters.rsaDetail.exponent))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "RSA exponent too large for TPM2 public key object.");

        *output = (TPM2B_PUBLIC) {
                .size = sizeof(TPMT_PUBLIC),
                .publicArea = {
                        .type = TPM2_ALG_RSA,
                        .nameAlg = TPM2_ALG_SHA256,
                        .objectAttributes = TPMA_OBJECT_DECRYPT | TPMA_OBJECT_SIGN_ENCRYPT | TPMA_OBJECT_USERWITHAUTH,
                        .parameters.rsaDetail = {
                                .scheme = {
                                        .scheme = TPM2_ALG_NULL,
                                        .details.anySig.hashAlg = TPM2_ALG_NULL,
                                },
                                .symmetric = {
                                        .algorithm = TPM2_ALG_NULL,
                                        .mode.sym = TPM2_ALG_NULL,
                                },
                                .keyBits = n_bytes * 8,
                                /* .exponent will be filled in below. */
                        },
                        .unique = {
                                .rsa.size = n_bytes,
                                /* .rsa.buffer will be filled in below. */
                        },
                },
        };

        if (BN_bn2bin(n, output->publicArea.unique.rsa.buffer) <= 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to convert RSA modulus.");

        if (BN_bn2bin(e, (unsigned char*) &output->publicArea.parameters.rsaDetail.exponent) <= 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to convert RSA exponent.");

        if (ret_fp) {
                _cleanup_free_ void *fp = NULL;
                size_t fp_size;

                assert(ret_fp_size);

                r = pubkey_fingerprint(input, EVP_sha256(), &fp, &fp_size);
                if (r < 0)
                        return log_error_errno(r, "Failed to calculate public key fingerprint: %m");

                *ret_fp = TAKE_PTR(fp);
                *ret_fp_size = fp_size;
        }

        return 0;
#else /* HAVE_OPENSSL */
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}

static int find_signature(
                JsonVariant *v,
                const TPML_PCR_SELECTION *pcr_selection,
                const void *fp,
                size_t fp_size,
                const void *policy,
                size_t policy_size,
                void *ret_signature,
                size_t *ret_signature_size) {

#if HAVE_OPENSSL
        JsonVariant *b, *i;
        const char *k;
        int r;

        /* Searches for a signature blob in the specified JSON object. Search keys are PCR bank, PCR mask,
         * public key, and policy digest. */

        if (!json_variant_is_object(v))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Signature is not a JSON object.");

        uint16_t pcr_bank = pcr_selection->pcrSelections[0].hash;
        uint32_t pcr_mask;
        r = tpm2_tpml_pcr_selection_to_mask(pcr_selection, pcr_bank, &pcr_mask);
        if (r < 0)
                return r;

        k = tpm2_hash_alg_to_string(pcr_bank);
        if (!k)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Don't know PCR bank %" PRIu16, pcr_bank);

        /* First, find field by bank */
        b = json_variant_by_key(v, k);
        if (!b)
                return log_error_errno(SYNTHETIC_ERRNO(ENXIO), "Signature lacks data for PCR bank '%s'.", k);

        if (!json_variant_is_array(b))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Bank data is not a JSON array.");

        /* Now iterate through all signatures known for this bank */
        JSON_VARIANT_ARRAY_FOREACH(i, b) {
                _cleanup_free_ void *fpj_data = NULL, *polj_data = NULL;
                JsonVariant *maskj, *fpj, *sigj, *polj;
                size_t fpj_size, polj_size;
                uint32_t parsed_mask;

                if (!json_variant_is_object(i))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Bank data element is not a JSON object");

                /* Check if the PCR mask matches our expectations */
                maskj = json_variant_by_key(i, "pcrs");
                if (!maskj)
                        continue;

                r = tpm2_parse_pcr_json_array(maskj, &parsed_mask);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse JSON PCR mask");

                if (parsed_mask != pcr_mask)
                        continue; /* Not for this PCR mask */

                /* Then check if this is for the public key we operate with */
                fpj = json_variant_by_key(i, "pkfp");
                if (!fpj)
                        continue;

                r = json_variant_unhex(fpj, &fpj_data, &fpj_size);
                if (r < 0)
                        return log_error_errno(r, "Failed to decode fingerprint in JSON data: %m");

                if (memcmp_nn(fp, fp_size, fpj_data, fpj_size) != 0)
                        continue; /* Not for this public key */

                /* Finally, check if this is for the PCR policy we expect this to be */
                polj = json_variant_by_key(i, "pol");
                if (!polj)
                        continue;

                r = json_variant_unhex(polj, &polj_data, &polj_size);
                if (r < 0)
                        return log_error_errno(r, "Failed to decode policy hash JSON data: %m");

                if (memcmp_nn(policy, policy_size, polj_data, polj_size) != 0)
                        continue;

                /* This entry matches all our expectations, now return the signature included in it */
                sigj = json_variant_by_key(i, "sig");
                if (!sigj)
                        continue;

                return json_variant_unbase64(sigj, ret_signature, ret_signature_size);
        }

        return log_error_errno(SYNTHETIC_ERRNO(ENXIO), "Couldn't find signature for this PCR bank, PCR index and public key.");
#else /* HAVE_OPENSSL */
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}

/* Calculates the "name" of a public key.
 *
 * As specified in TPM2 spec "Part 1: Architecture", a key's "name" is its nameAlg value followed by a hash
 * of its TPM2 public area, all properly marshalled. This allows a key's "name" to be dependent not only on
 * the key fingerprint, but also on the TPM2-specific fields that associated with the key (i.e. all fields in
 * TPMT_PUBLIC). Note that this means an existing key may not change any of its TPMT_PUBLIC fields, since
 * that would also change the key name.
 *
 * Since we (currently) hardcode to always using SHA256 for hashing, this returns an error if the public key
 * nameAlg is not TPM2_ALG_SHA256. */
int tpm2_calculate_name(const TPMT_PUBLIC *public, TPM2B_NAME *ret_name) {
        TSS2_RC rc;
        int r;

        assert(public);
        assert(ret_name);

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        if (public->nameAlg != TPM2_ALG_SHA256)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Unsupported nameAlg: 0x%x",
                                       public->nameAlg);

        _cleanup_free_ uint8_t *buf = NULL;
        size_t size = 0;

        buf = (uint8_t*) new(TPMT_PUBLIC, 1);
        if (!buf)
                return log_oom();

        rc = sym_Tss2_MU_TPMT_PUBLIC_Marshal(public, buf, sizeof(TPMT_PUBLIC), &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal public key: %s", sym_Tss2_RC_Decode(rc));

        TPM2B_DIGEST digest = {};
        r = tpm2_digest_buffer(TPM2_ALG_SHA256, &digest, buf, size, /* extend= */ false);
        if (r < 0)
                return r;

        TPMT_HA ha = {
                .hashAlg = TPM2_ALG_SHA256,
        };
        assert(digest.size <= sizeof(ha.digest.sha256));
        memcpy_safe(ha.digest.sha256, digest.buffer, digest.size);

        TPM2B_NAME name;
        size = 0;
        rc = sym_Tss2_MU_TPMT_HA_Marshal(&ha, name.name, sizeof(name.name), &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal key name: %s", sym_Tss2_RC_Decode(rc));
        name.size = size;

        tpm2_log_debug_name(&name, "Calculated name");

        *ret_name = name;

        return 0;
}

/* Get the "name" of a key from the TPM.
 *
 * The "name" of a key is explained above in tpm2_calculate_name().
 *
 * The handle must reference a key already present in the TPM. It may be either a public key only, or a
 * public/private keypair. */
static int tpm2_get_name(
                Tpm2Context *c,
                const Tpm2Handle *handle,
                TPM2B_NAME **ret_name) {

        _cleanup_(Esys_Freep) TPM2B_NAME *name = NULL;
        TSS2_RC rc;

        assert(c);
        assert(handle);
        assert(ret_name);

        rc = sym_Esys_TR_GetName(c->esys_context, handle->esys_handle, &name);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to get name of public key from TPM: %s", sym_Tss2_RC_Decode(rc));

        tpm2_log_debug_name(name, "Object name");

        *ret_name = TAKE_PTR(name);

        return 0;
}

/* Extend 'digest' with the PolicyAuthValue calculated hash. */
int tpm2_calculate_policy_auth_value(TPM2B_DIGEST *digest) {
        TPM2_CC command = TPM2_CC_PolicyAuthValue;
        TSS2_RC rc;
        int r;

        assert(digest);
        assert(digest->size == SHA256_DIGEST_SIZE);

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        uint8_t buf[sizeof(command)];
        size_t offset = 0;

        rc = sym_Tss2_MU_TPM2_CC_Marshal(command, buf, sizeof(buf), &offset);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal PolicyAuthValue command: %s", sym_Tss2_RC_Decode(rc));

        if (offset != sizeof(command))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Offset 0x%zx wrong after marshalling PolicyAuthValue command", offset);

        r = tpm2_digest_buffer(TPM2_ALG_SHA256, digest, buf, offset, /* extend= */ true);
        if (r < 0)
                return r;

        tpm2_log_debug_digest(digest, "PolicyAuthValue calculated digest");

        return 0;
}

static int tpm2_policy_auth_value(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2B_DIGEST **ret_policy_digest) {

        TSS2_RC rc;

        assert(c);
        assert(session);

        log_debug("Adding authValue policy.");

        rc = sym_Esys_PolicyAuthValue(
                        c->esys_context,
                        session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to add authValue policy to TPM: %s",
                                       sym_Tss2_RC_Decode(rc));

        return tpm2_get_policy_digest(c, session, ret_policy_digest);
}

/* Extend 'digest' with the PolicyPCR calculated hash. */
int tpm2_calculate_policy_pcr(
                const TPML_PCR_SELECTION *pcr_selection,
                const TPM2B_DIGEST pcr_values[],
                size_t n_pcr_values,
                TPM2B_DIGEST *digest) {

        TPM2_CC command = TPM2_CC_PolicyPCR;
        TSS2_RC rc;
        int r;

        assert(pcr_selection);
        assert(pcr_values || n_pcr_values == 0);
        assert(digest);
        assert(digest->size == SHA256_DIGEST_SIZE);

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        TPM2B_DIGEST hash = {};
        r = tpm2_digest_many_digests(TPM2_ALG_SHA256, &hash, pcr_values, n_pcr_values, /* extend= */ false);
        if (r < 0)
                return r;

        _cleanup_free_ uint8_t *buf = NULL;
        size_t size = 0, maxsize = sizeof(command) + sizeof(*pcr_selection);

        buf = malloc(maxsize);
        if (!buf)
                return log_oom();

        rc = sym_Tss2_MU_TPM2_CC_Marshal(command, buf, maxsize, &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal PolicyPCR command: %s", sym_Tss2_RC_Decode(rc));

        rc = sym_Tss2_MU_TPML_PCR_SELECTION_Marshal(pcr_selection, buf, maxsize, &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal PCR selection: %s", sym_Tss2_RC_Decode(rc));

        struct iovec data[] = {
                IOVEC_MAKE(buf, size),
                IOVEC_MAKE(hash.buffer, hash.size),
        };
        r = tpm2_digest_many(TPM2_ALG_SHA256, digest, data, ELEMENTSOF(data), /* extend= */ true);
        if (r < 0)
                return r;

        tpm2_log_debug_digest(digest, "PolicyPCR calculated digest");

        return 0;
}

static int tpm2_policy_pcr(
                Tpm2Context *c,
                const Tpm2Handle *session,
                const TPML_PCR_SELECTION *pcr_selection,
                TPM2B_DIGEST **ret_policy_digest) {

        TSS2_RC rc;

        assert(c);
        assert(session);
        assert(pcr_selection);

        log_debug("Adding PCR hash policy.");

        rc = sym_Esys_PolicyPCR(
                        c->esys_context,
                        session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        NULL,
                        pcr_selection);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to add PCR policy to TPM: %s", sym_Tss2_RC_Decode(rc));

        return tpm2_get_policy_digest(c, session, ret_policy_digest);
}

/* Extend 'digest' with the PolicyAuthorize calculated hash. */
int tpm2_calculate_policy_authorize(
                const TPM2B_PUBLIC *public,
                const TPM2B_DIGEST *policy_ref,
                TPM2B_DIGEST *digest) {

        TPM2_CC command = TPM2_CC_PolicyAuthorize;
        TSS2_RC rc;
        int r;

        assert(public);
        assert(digest);
        assert(digest->size == SHA256_DIGEST_SIZE);

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        uint8_t buf[sizeof(command)];
        size_t offset = 0;

        rc = sym_Tss2_MU_TPM2_CC_Marshal(command, buf, sizeof(buf), &offset);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal PolicyAuthorize command: %s", sym_Tss2_RC_Decode(rc));

        if (offset != sizeof(command))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Offset 0x%zx wrong after marshalling PolicyAuthorize command", offset);

        TPM2B_NAME name = {};
        r = tpm2_calculate_name(&public->publicArea, &name);
        if (r < 0)
                return r;

        /* PolicyAuthorize does not use the previous hash value; we must zero and then extend it. */
        zero(digest->buffer);

        struct iovec data[] = {
                IOVEC_MAKE(buf, offset),
                IOVEC_MAKE(name.name, name.size),
        };
        r = tpm2_digest_many(TPM2_ALG_SHA256, digest, data, ELEMENTSOF(data), /* extend= */ true);
        if (r < 0)
                return r;

        /* PolicyAuthorize requires hashing twice; this is either an extension or rehashing. */
        if (policy_ref)
                r = tpm2_digest_many_digests(TPM2_ALG_SHA256, digest, policy_ref, 1, /* extend= */ true);
        else
                r = tpm2_digest_rehash(TPM2_ALG_SHA256, digest);
        if (r < 0)
                return r;

        tpm2_log_debug_digest(digest, "PolicyAuthorize calculated digest");

        return 0;
}

static int tpm2_policy_authorize(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPML_PCR_SELECTION *pcr_selection,
                const TPM2B_PUBLIC *public,
                const void *fp,
                size_t fp_size,
                JsonVariant *signature_json,
                TPM2B_DIGEST **ret_policy_digest) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(session);
        assert(pcr_selection);
        assert(public);
        assert(fp && fp_size > 0);

        log_debug("Adding PCR signature policy.");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *pubkey_handle = NULL;
        r = tpm2_load_external(c, NULL, public, NULL, &pubkey_handle);
        if (r < 0)
                return r;

        /* Acquire the "name" of what we just loaded */
        _cleanup_(Esys_Freep) TPM2B_NAME *pubkey_name = NULL;
        r = tpm2_get_name(c, pubkey_handle, &pubkey_name);
        if (r < 0)
                return r;

        /* If we have a signature, proceed with verifying the PCR digest */
        const TPMT_TK_VERIFIED *check_ticket;
        _cleanup_(Esys_Freep) TPMT_TK_VERIFIED *check_ticket_buffer = NULL;
        _cleanup_(Esys_Freep) TPM2B_DIGEST *approved_policy = NULL;
        if (signature_json) {
                r = tpm2_policy_pcr(
                                c,
                                session,
                                pcr_selection,
                                &approved_policy);
                if (r < 0)
                        return r;

                _cleanup_free_ void *signature_raw = NULL;
                size_t signature_size;

                r = find_signature(
                                signature_json,
                                pcr_selection,
                                fp, fp_size,
                                approved_policy->buffer,
                                approved_policy->size,
                                &signature_raw,
                                &signature_size);
                if (r < 0)
                        return r;

                /* TPM2_VerifySignature() will only verify the RSA part of the RSA+SHA256 signature,
                 * hence we need to do the SHA256 part ourselves, first */
                TPM2B_DIGEST signature_hash = *approved_policy;
                r = tpm2_digest_rehash(TPM2_ALG_SHA256, &signature_hash);
                if (r < 0)
                        return r;

                TPMT_SIGNATURE policy_signature = {
                        .sigAlg = TPM2_ALG_RSASSA,
                        .signature.rsassa = {
                                .hash = TPM2_ALG_SHA256,
                                .sig.size = signature_size,
                        },
                };
                if (signature_size > sizeof(policy_signature.signature.rsassa.sig.buffer))
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Signature larger than buffer.");
                memcpy(policy_signature.signature.rsassa.sig.buffer, signature_raw, signature_size);

                rc = sym_Esys_VerifySignature(
                                c->esys_context,
                                pubkey_handle->esys_handle,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                &signature_hash,
                                &policy_signature,
                                &check_ticket_buffer);
                if (rc != TSS2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to validate signature in TPM: %s", sym_Tss2_RC_Decode(rc));

                check_ticket = check_ticket_buffer;
        } else {
                /* When enrolling, we pass a NULL ticket */
                static const TPMT_TK_VERIFIED check_ticket_null = {
                        .tag = TPM2_ST_VERIFIED,
                        .hierarchy = TPM2_RH_OWNER,
                };

                check_ticket = &check_ticket_null;
        }

        rc = sym_Esys_PolicyAuthorize(
                        c->esys_context,
                        session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        approved_policy,
                        /* policyRef= */ &(const TPM2B_NONCE) {},
                        pubkey_name,
                        check_ticket);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to push Authorize policy into TPM: %s", sym_Tss2_RC_Decode(rc));

        return tpm2_get_policy_digest(c, session, ret_policy_digest);
}

/* Extend 'digest' with the calculated policy hash. */
static int tpm2_calculate_sealing_policy(
                const TPML_PCR_SELECTION *hash_pcr_selection,
                const TPM2B_DIGEST *hash_pcr_values,
                size_t n_hash_pcr_values,
                const TPM2B_PUBLIC *public,
                const char *pin,
                TPM2B_DIGEST *digest) {

        int r;

        assert(digest);

        if (public) {
                r = tpm2_calculate_policy_authorize(public, NULL, digest);
                if (r < 0)
                        return r;
        }

        if (hash_pcr_selection && !tpm2_tpml_pcr_selection_is_empty(hash_pcr_selection)) {
                r = tpm2_calculate_policy_pcr(hash_pcr_selection, hash_pcr_values, n_hash_pcr_values, digest);
                if (r < 0)
                        return r;
        }

        if (pin) {
                r = tpm2_calculate_policy_auth_value(digest);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int tpm2_build_sealing_policy(
                Tpm2Context *c,
                const Tpm2Handle *session,
                uint32_t hash_pcr_mask,
                uint16_t pcr_bank,
                const TPM2B_PUBLIC *public,
                const void *fp,
                size_t fp_size,
                uint32_t pubkey_pcr_mask,
                JsonVariant *signature_json,
                bool use_pin,
                TPM2B_DIGEST **ret_policy_digest) {

        int r;

        assert(c);
        assert(session);
        assert(pubkey_pcr_mask == 0 || public);

        log_debug("Building sealing policy.");

        if ((hash_pcr_mask | pubkey_pcr_mask) != 0) {
                r = tpm2_pcr_mask_good(c, pcr_bank, hash_pcr_mask|pubkey_pcr_mask);
                if (r < 0)
                        return r;
                if (r == 0)
                        log_warning("Selected TPM2 PCRs are not initialized on this system.");
        }

        if (pubkey_pcr_mask != 0) {
                TPML_PCR_SELECTION pcr_selection;
                tpm2_tpml_pcr_selection_from_mask(pubkey_pcr_mask, (TPMI_ALG_HASH)pcr_bank, &pcr_selection);
                r = tpm2_policy_authorize(c, session, &pcr_selection, public, fp, fp_size, signature_json, NULL);
                if (r < 0)
                        return r;
        }

        if (hash_pcr_mask != 0) {
                TPML_PCR_SELECTION pcr_selection;
                tpm2_tpml_pcr_selection_from_mask(hash_pcr_mask, (TPMI_ALG_HASH)pcr_bank, &pcr_selection);
                r = tpm2_policy_pcr(c, session, &pcr_selection, NULL);
                if (r < 0)
                        return r;
        }

        if (use_pin) {
                r = tpm2_policy_auth_value(c, session, NULL);
                if (r < 0)
                        return r;
        }

        r = tpm2_get_policy_digest(c, session, ret_policy_digest);
        if (r < 0)
                return r;

        return 0;
}

int tpm2_seal(const char *device,
              uint32_t hash_pcr_mask,
              const void *pubkey,
              const size_t pubkey_size,
              uint32_t pubkey_pcr_mask,
              const char *pin,
              void **ret_secret,
              size_t *ret_secret_size,
              void **ret_blob,
              size_t *ret_blob_size,
              void **ret_pcr_hash,
              size_t *ret_pcr_hash_size,
              uint16_t *ret_pcr_bank,
              uint16_t *ret_primary_alg,
              void **ret_srk_buf,
              size_t *ret_srk_buf_size) {

        TSS2_RC rc;
        int r;

        assert(pubkey || pubkey_size == 0);

        assert(ret_secret);
        assert(ret_secret_size);
        assert(ret_blob);
        assert(ret_blob_size);
        assert(ret_pcr_hash);
        assert(ret_pcr_hash_size);
        assert(ret_pcr_bank);

        assert(TPM2_PCR_MASK_VALID(hash_pcr_mask));
        assert(TPM2_PCR_MASK_VALID(pubkey_pcr_mask));

        /* So here's what we do here: we connect to the TPM2 chip. It persistently contains a "seed" key that
         * is randomized when the TPM2 is first initialized or reset and remains stable across boots. We
         * generate a "primary" key pair derived from that (ECC if possible, RSA as fallback). Given the seed
         * remains fixed this will result in the same key pair whenever we specify the exact same parameters
         * for it. We then create a PCR-bound policy session, which calculates a hash on the current PCR
         * values of the indexes we specify. We then generate a randomized key on the host (which is the key
         * we actually enroll in the LUKS2 keyslots), which we upload into the TPM2, where it is encrypted
         * with the "primary" key, taking the PCR policy session into account. We then download the encrypted
         * key from the TPM2 ("sealing") and marshall it into binary form, which is ultimately placed in the
         * LUKS2 JSON header.
         *
         * The TPM2 "seed" key and "primary" keys never leave the TPM2 chip (and cannot be extracted at
         * all). The random key we enroll in LUKS2 we generate on the host using the Linux random device. It
         * is stored in the LUKS2 JSON only in encrypted form with the "primary" key of the TPM2 chip, thus
         * binding the unlocking to the TPM2 chip. */

        usec_t start = now(CLOCK_MONOTONIC);

        _cleanup_(tpm2_context_unrefp) Tpm2Context *c = NULL;
        r = tpm2_context_new(device, &c);
        if (r < 0)
                return r;

        TPMI_ALG_HASH pcr_bank = 0;
        if (hash_pcr_mask | pubkey_pcr_mask) {
                /* Some TPM2 devices only can do SHA1. Prefer SHA256 but allow SHA1. */
                r = tpm2_get_best_pcr_bank(c, hash_pcr_mask|pubkey_pcr_mask, &pcr_bank);
                if (r < 0)
                        return r;
        }

        TPML_PCR_SELECTION hash_pcr_selection = {};
        _cleanup_free_ TPM2B_DIGEST *hash_pcr_values = NULL;
        size_t n_hash_pcr_values = 0;
        if (hash_pcr_mask) {
                /* For now, we just read the current values from the system; we need to be able to specify
                 * expected values, eventually. */
                tpm2_tpml_pcr_selection_from_mask(hash_pcr_mask, pcr_bank, &hash_pcr_selection);
                r = tpm2_pcr_read(c, &hash_pcr_selection, &hash_pcr_selection, &hash_pcr_values, &n_hash_pcr_values);
                if (r < 0)
                        return r;
        }

        TPM2B_PUBLIC pubkey_tpm2, *authorize_key = NULL;
        if (pubkey) {
                r = openssl_pubkey_to_tpm2_pubkey(pubkey, pubkey_size, &pubkey_tpm2, NULL, NULL);
                if (r < 0)
                        return r;
                authorize_key = &pubkey_tpm2;
        }

        TPM2B_DIGEST policy_digest;
        r = tpm2_digest_init(TPM2_ALG_SHA256, &policy_digest);
        if (r < 0)
                return r;

        r = tpm2_calculate_sealing_policy(
                        &hash_pcr_selection,
                        hash_pcr_values,
                        n_hash_pcr_values,
                        authorize_key,
                        pin,
                        &policy_digest);
        if (r < 0)
                return r;

        /* We use a keyed hash object (i.e. HMAC) to store the secret key we want to use for unlocking the
         * LUKS2 volume with. We don't ever use for HMAC/keyed hash operations however, we just use it
         * because it's a key type that is universally supported and suitable for symmetric binary blobs. */
        TPMT_PUBLIC hmac_template = {
                .type = TPM2_ALG_KEYEDHASH,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT,
                .parameters.keyedHashDetail.scheme.scheme = TPM2_ALG_NULL,
                .unique.keyedHash.size = SHA256_DIGEST_SIZE,
                .authPolicy = policy_digest,
        };

        TPMS_SENSITIVE_CREATE hmac_sensitive = {
                .data.size = hmac_template.unique.keyedHash.size,
        };

        CLEANUP_ERASE(hmac_sensitive);

        if (pin) {
                r = tpm2_get_pin_auth(TPM2_ALG_SHA256, pin, &hmac_sensitive.userAuth);
                if (r < 0)
                        return r;
        }

        assert(sizeof(hmac_sensitive.data.buffer) >= hmac_sensitive.data.size);

        (void) tpm2_credit_random(c);

        log_debug("Generating secret key data.");

        r = crypto_random_bytes(hmac_sensitive.data.buffer, hmac_sensitive.data.size);
        if (r < 0)
                return log_error_errno(r, "Failed to generate secret key: %m");

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *primary_public = NULL;
        _cleanup_(tpm2_handle_freep) Tpm2Handle *primary_handle = NULL;
        if (ret_srk_buf) {
                r = tpm2_get_or_create_srk(c, NULL, &primary_public, NULL, NULL, &primary_handle);
                if (r < 0)
                        return r;
        } else {
                /* TODO: force all callers to provide ret_srk_buf, so we can stop sealing with the legacy templates. */
                TPM2B_PUBLIC template = { .size = sizeof(TPMT_PUBLIC), };
                r = tpm2_get_legacy_template(TPM2_ALG_ECC, &template.publicArea);
                if (r < 0)
                        return log_error_errno(r, "Could not get legacy ECC template: %m");

                if (!tpm2_supports_tpmt_public(c, &template.publicArea)) {
                        r = tpm2_get_legacy_template(TPM2_ALG_RSA, &template.publicArea);
                        if (r < 0)
                                return log_error_errno(r, "Could not get legacy RSA template: %m");

                        if (!tpm2_supports_tpmt_public(c, &template.publicArea))
                                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                                       "TPM does not support either ECC or RSA legacy template.");
                }

                r = tpm2_create_primary(
                                c,
                                /* session= */ NULL,
                                &template,
                                /* sensitive= */ NULL,
                                &primary_public,
                                &primary_handle);
                if (r < 0)
                        return r;
        }

        _cleanup_(tpm2_handle_freep) Tpm2Handle *encryption_session = NULL;
        r = tpm2_make_encryption_session(c, primary_handle, &TPM2_HANDLE_NONE, &encryption_session);
        if (r < 0)
                return r;

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        _cleanup_(Esys_Freep) TPM2B_PRIVATE *private = NULL;
        r = tpm2_create(c, primary_handle, encryption_session, &hmac_template, &hmac_sensitive, &public, &private);
        if (r < 0)
                return r;

        _cleanup_(erase_and_freep) void *secret = NULL;
        secret = memdup(hmac_sensitive.data.buffer, hmac_sensitive.data.size);
        if (!secret)
                return log_oom();

        log_debug("Marshalling private and public part of HMAC key.");

        _cleanup_free_ void *blob = NULL;
        size_t max_size = sizeof(*private) + sizeof(*public), blob_size = 0;

        blob = malloc0(max_size);
        if (!blob)
                return log_oom();

        rc = sym_Tss2_MU_TPM2B_PRIVATE_Marshal(private, blob, max_size, &blob_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal private key: %s", sym_Tss2_RC_Decode(rc));

        rc = sym_Tss2_MU_TPM2B_PUBLIC_Marshal(public, blob, max_size, &blob_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal public key: %s", sym_Tss2_RC_Decode(rc));

        _cleanup_free_ void *hash = NULL;
        hash = memdup(policy_digest.buffer, policy_digest.size);
        if (!hash)
                return log_oom();

        /* serialize the key for storage in the LUKS header. A deserialized ESYS_TR provides both
         * the raw TPM handle as well as the object name. The object name is used to verify that
         * the key we use later is the key we expect to establish the session with.
         */
        _cleanup_(Esys_Freep) uint8_t *srk_buf = NULL;
        size_t srk_buf_size = 0;
        if (ret_srk_buf) {
                log_debug("Serializing SRK ESYS_TR reference");
                rc = sym_Esys_TR_Serialize(c->esys_context, primary_handle->esys_handle, &srk_buf, &srk_buf_size);
                if (rc != TSS2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                            "Failed to serialize primary key: %s", sym_Tss2_RC_Decode(rc));
        }

        if (DEBUG_LOGGING)
                log_debug("Completed TPM2 key sealing in %s.", FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - start, 1));

        if (ret_srk_buf) {
                /*
                 * make a copy since we don't want the caller to understand that
                 * ESYS allocated the pointer. It would make tracking what deallocator
                 * to use for srk_buf in which context a PITA.
                 */
                void *tmp = memdup(srk_buf, srk_buf_size);
                if (!tmp)
                        return log_oom();

                *ret_srk_buf = TAKE_PTR(tmp);
                *ret_srk_buf_size = srk_buf_size;
        }

        *ret_secret = TAKE_PTR(secret);
        *ret_secret_size = hmac_sensitive.data.size;
        *ret_blob = TAKE_PTR(blob);
        *ret_blob_size = blob_size;
        *ret_pcr_hash = TAKE_PTR(hash);
        *ret_pcr_hash_size = policy_digest.size;
        *ret_pcr_bank = pcr_bank;
        *ret_primary_alg = primary_public->publicArea.type;

        return 0;
}

#define RETRY_UNSEAL_MAX 30u

int tpm2_unseal(const char *device,
                uint32_t hash_pcr_mask,
                uint16_t pcr_bank,
                const void *pubkey,
                size_t pubkey_size,
                uint32_t pubkey_pcr_mask,
                JsonVariant *signature,
                const char *pin,
                uint16_t primary_alg,
                const void *blob,
                size_t blob_size,
                const void *known_policy_hash,
                size_t known_policy_hash_size,
                const void *srk_buf,
                size_t srk_buf_size,
                void **ret_secret,
                size_t *ret_secret_size) {

        TSS2_RC rc;
        int r;

        assert(blob);
        assert(blob_size > 0);
        assert(known_policy_hash_size == 0 || known_policy_hash);
        assert(pubkey_size == 0 || pubkey);
        assert(ret_secret);
        assert(ret_secret_size);

        assert(TPM2_PCR_MASK_VALID(hash_pcr_mask));
        assert(TPM2_PCR_MASK_VALID(pubkey_pcr_mask));

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support is not installed.");

        /* So here's what we do here: We connect to the TPM2 chip. As we do when sealing we generate a
         * "primary" key on the TPM2 chip, with the same parameters as well as a PCR-bound policy session.
         * Given we pass the same parameters, this will result in the same "primary" key, and same policy
         * hash (the latter of course, only if the PCR values didn't change in between). We unmarshal the
         * encrypted key we stored in the LUKS2 JSON token header and upload it into the TPM2, where it is
         * decrypted if the seed and the PCR policy were right ("unsealing"). We then download the result,
         * and use it to unlock the LUKS2 volume. */

        usec_t start = now(CLOCK_MONOTONIC);

        log_debug("Unmarshalling private part of HMAC key.");

        TPM2B_PRIVATE private = {};
        size_t offset = 0;
        rc = sym_Tss2_MU_TPM2B_PRIVATE_Unmarshal(blob, blob_size, &offset, &private);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to unmarshal private key: %s", sym_Tss2_RC_Decode(rc));

        log_debug("Unmarshalling public part of HMAC key.");

        TPM2B_PUBLIC public = {};
        rc = sym_Tss2_MU_TPM2B_PUBLIC_Unmarshal(blob, blob_size, &offset, &public);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to unmarshal public key: %s", sym_Tss2_RC_Decode(rc));

        _cleanup_(tpm2_context_unrefp) Tpm2Context *c = NULL;
        r = tpm2_context_new(device, &c);
        if (r < 0)
                return r;

        /* Older code did not save the pcr_bank, and unsealing needed to detect the best pcr bank to use,
         * so we need to handle that legacy situation. */
        if (pcr_bank == UINT16_MAX) {
                r = tpm2_get_best_pcr_bank(c, hash_pcr_mask|pubkey_pcr_mask, &pcr_bank);
                if (r < 0)
                        return r;
        }

        _cleanup_(tpm2_handle_freep) Tpm2Handle *primary_handle = NULL;
        if (srk_buf) {
                r = tpm2_handle_new(c, &primary_handle);
                if (r < 0)
                        return r;

                primary_handle->flush = false;

                log_debug("Found existing SRK key to use, deserializing ESYS_TR");
                rc = sym_Esys_TR_Deserialize(
                                c->esys_context,
                                srk_buf,
                                srk_buf_size,
                                &primary_handle->esys_handle);
                if (rc != TSS2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to deserialize primary key: %s", sym_Tss2_RC_Decode(rc));
        } else if (primary_alg != 0) {
                TPM2B_PUBLIC template = { .size = sizeof(TPMT_PUBLIC), };
                r = tpm2_get_legacy_template(primary_alg, &template.publicArea);
                if (r < 0)
                        return log_error_errno(r, "Could not get legacy template: %m");

                r = tpm2_create_primary(
                                c,
                                /* session= */ NULL,
                                &template,
                                /* sensitive= */ NULL,
                                /* ret_public= */ NULL,
                                &primary_handle);
                if (r < 0)
                        return r;
        } else
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "No SRK or primary alg provided.");

        log_debug("Loading HMAC key into TPM.");

        /*
         * Nothing sensitive on the bus, no need for encryption. Even if an attacker
         * gives you back a different key, the session initiation will fail. In the
         * SRK model, the tpmKey is verified. In the non-srk model, with pin, the bindKey
         * provides protections.
         */
        _cleanup_(tpm2_handle_freep) Tpm2Handle *hmac_key = NULL;
        r = tpm2_load(c, primary_handle, NULL, &public, &private, &hmac_key);
        if (r < 0)
                return r;

        TPM2B_PUBLIC pubkey_tpm2, *authorize_key = NULL;
        _cleanup_free_ void *fp = NULL;
        size_t fp_size = 0;
        if (pubkey) {
                r = openssl_pubkey_to_tpm2_pubkey(pubkey, pubkey_size, &pubkey_tpm2, &fp, &fp_size);
                if (r < 0)
                        return r;
                authorize_key = &pubkey_tpm2;
        }

        /*
         * if a pin is set for the seal object, use it to bind the session
         * key to that object. This prevents active bus interposers from
         * faking a TPM and seeing the unsealed value. An active interposer
         * could fake a TPM, satisfying the encrypted session, and just
         * forward everything to the *real* TPM.
         */
        r = tpm2_set_auth(c, hmac_key, pin);
        if (r < 0)
                return r;

        _cleanup_(Esys_Freep) TPM2B_SENSITIVE_DATA* unsealed = NULL;
        for (unsigned i = RETRY_UNSEAL_MAX;; i--) {
                _cleanup_(tpm2_handle_freep) Tpm2Handle *encryption_session = NULL;
                r = tpm2_make_encryption_session(c, primary_handle, hmac_key, &encryption_session);
                if (r < 0)
                        return r;

                _cleanup_(tpm2_handle_freep) Tpm2Handle *policy_session = NULL;
                _cleanup_(Esys_Freep) TPM2B_DIGEST *policy_digest = NULL;
                r = tpm2_make_policy_session(
                                c,
                                primary_handle,
                                encryption_session,
                                /* trial= */ false,
                                &policy_session);
                if (r < 0)
                        return r;

                r = tpm2_build_sealing_policy(
                                c,
                                policy_session,
                                hash_pcr_mask,
                                pcr_bank,
                                authorize_key,
                                fp, fp_size,
                                pubkey_pcr_mask,
                                signature,
                                !!pin,
                                &policy_digest);
                if (r < 0)
                        return r;

                /* If we know the policy hash to expect, and it doesn't match, we can shortcut things here, and not
                 * wait until the TPM2 tells us to go away. */
                if (known_policy_hash_size > 0 &&
                        memcmp_nn(policy_digest->buffer, policy_digest->size, known_policy_hash, known_policy_hash_size) != 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EPERM),
                                                       "Current policy digest does not match stored policy digest, cancelling "
                                                       "TPM2 authentication attempt.");

                log_debug("Unsealing HMAC key.");

                rc = sym_Esys_Unseal(
                                c->esys_context,
                                hmac_key->esys_handle,
                                policy_session->esys_handle,
                                encryption_session->esys_handle, /* use HMAC session to enable parameter encryption */
                                ESYS_TR_NONE,
                                &unsealed);
                if (rc == TSS2_RC_SUCCESS)
                        break;
                if (rc != TPM2_RC_PCR_CHANGED || i == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to unseal HMAC key in TPM: %s", sym_Tss2_RC_Decode(rc));
                log_debug("A PCR value changed during the TPM2 policy session, restarting HMAC key unsealing (%u tries left).", i);
        }

        _cleanup_(erase_and_freep) char *secret = NULL;
        secret = memdup(unsealed->buffer, unsealed->size);
        explicit_bzero_safe(unsealed->buffer, unsealed->size);
        if (!secret)
                return log_oom();

        if (DEBUG_LOGGING)
                log_debug("Completed TPM2 key unsealing in %s.", FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - start, 1));

        *ret_secret = TAKE_PTR(secret);
        *ret_secret_size = unsealed->size;

        return 0;
}

#endif

int tpm2_list_devices(void) {
#if HAVE_TPM2
        _cleanup_(table_unrefp) Table *t = NULL;
        _cleanup_closedir_ DIR *d = NULL;
        int r;

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support is not installed.");

        t = table_new("path", "device", "driver");
        if (!t)
                return log_oom();

        d = opendir("/sys/class/tpmrm");
        if (!d) {
                log_full_errno(errno == ENOENT ? LOG_DEBUG : LOG_ERR, errno, "Failed to open /sys/class/tpmrm: %m");
                if (errno != ENOENT)
                        return -errno;
        } else {
                for (;;) {
                        _cleanup_free_ char *device_path = NULL, *device = NULL, *driver_path = NULL, *driver = NULL, *node = NULL;
                        struct dirent *de;

                        de = readdir_no_dot(d);
                        if (!de)
                                break;

                        device_path = path_join("/sys/class/tpmrm", de->d_name, "device");
                        if (!device_path)
                                return log_oom();

                        r = readlink_malloc(device_path, &device);
                        if (r < 0)
                                log_debug_errno(r, "Failed to read device symlink %s, ignoring: %m", device_path);
                        else {
                                driver_path = path_join(device_path, "driver");
                                if (!driver_path)
                                        return log_oom();

                                r = readlink_malloc(driver_path, &driver);
                                if (r < 0)
                                        log_debug_errno(r, "Failed to read driver symlink %s, ignoring: %m", driver_path);
                        }

                        node = path_join("/dev", de->d_name);
                        if (!node)
                                return log_oom();

                        r = table_add_many(
                                        t,
                                        TABLE_PATH, node,
                                        TABLE_STRING, device ? last_path_component(device) : NULL,
                                        TABLE_STRING, driver ? last_path_component(driver) : NULL);
                        if (r < 0)
                                return table_log_add_error(r);
                }
        }

        if (table_get_rows(t) <= 1) {
                log_info("No suitable TPM2 devices found.");
                return 0;
        }

        r = table_print(t, stdout);
        if (r < 0)
                return log_error_errno(r, "Failed to show device table: %m");

        return 0;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "TPM2 not supported on this build.");
#endif
}

int tpm2_find_device_auto(
                int log_level, /* log level when no device is found */
                char **ret) {
#if HAVE_TPM2
        _cleanup_closedir_ DIR *d = NULL;
        int r;

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support is not installed.");

        d = opendir("/sys/class/tpmrm");
        if (!d) {
                log_full_errno(errno == ENOENT ? LOG_DEBUG : LOG_ERR, errno,
                               "Failed to open /sys/class/tpmrm: %m");
                if (errno != ENOENT)
                        return -errno;
        } else {
                _cleanup_free_ char *node = NULL;

                for (;;) {
                        struct dirent *de;

                        de = readdir_no_dot(d);
                        if (!de)
                                break;

                        if (node)
                                return log_error_errno(SYNTHETIC_ERRNO(ENOTUNIQ),
                                                       "More than one TPM2 (tpmrm) device found.");

                        node = path_join("/dev", de->d_name);
                        if (!node)
                                return log_oom();
                }

                if (node) {
                        *ret = TAKE_PTR(node);
                        return 0;
                }
        }

        return log_full_errno(log_level, SYNTHETIC_ERRNO(ENODEV), "No TPM2 (tpmrm) device found.");
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "TPM2 not supported on this build.");
#endif
}

#if HAVE_TPM2
int tpm2_extend_bytes(
                Tpm2Context *c,
                char **banks,
                unsigned pcr_index,
                const void *data,
                size_t data_size,
                const void *secret,
                size_t secret_size) {

#if HAVE_OPENSSL
        TPML_DIGEST_VALUES values = {};
        TSS2_RC rc;

        assert(c);
        assert(data || data_size == 0);
        assert(secret || secret_size == 0);

        if (data_size == SIZE_MAX)
                data_size = strlen(data);
        if (secret_size == SIZE_MAX)
                secret_size = strlen(secret);

        if (pcr_index >= TPM2_PCRS_MAX)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Can't measure into unsupported PCR %u, refusing.", pcr_index);

        if (strv_isempty(banks))
                return 0;

        STRV_FOREACH(bank, banks) {
                const EVP_MD *implementation;
                int id;

                assert_se(implementation = EVP_get_digestbyname(*bank));

                if (values.count >= ELEMENTSOF(values.digests))
                        return log_error_errno(SYNTHETIC_ERRNO(E2BIG), "Too many banks selected.");

                if ((size_t) EVP_MD_size(implementation) > sizeof(values.digests[values.count].digest))
                        return log_error_errno(SYNTHETIC_ERRNO(E2BIG), "Hash result too large for TPM2.");

                id = tpm2_hash_alg_from_string(EVP_MD_name(implementation));
                if (id < 0)
                        return log_error_errno(id, "Can't map hash name to TPM2.");

                values.digests[values.count].hashAlg = id;

                /* So here's a twist: sometimes we want to measure secrets (e.g. root file system volume
                 * key), but we'd rather not leak a literal hash of the secret to the TPM (given that the
                 * wire is unprotected, and some other subsystem might use the simple, literal hash of the
                 * secret for other purposes, maybe because it needs a shorter secret derived from it for
                 * some unrelated purpose, who knows). Hence we instead measure an HMAC signature of a
                 * private non-secret string instead. */
                if (secret_size > 0) {
                        if (!HMAC(implementation, secret, secret_size, data, data_size, (unsigned char*) &values.digests[values.count].digest, NULL))
                                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Failed to calculate HMAC of data to measure.");
                } else if (EVP_Digest(data, data_size, (unsigned char*) &values.digests[values.count].digest, NULL, implementation, NULL) != 1)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Failed to hash data to measure.");

                values.count++;
        }

        rc = sym_Esys_PCR_Extend(
                        c->esys_context,
                        ESYS_TR_PCR0 + pcr_index,
                        ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &values);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(
                                SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                "Failed to measure into PCR %u: %s",
                                pcr_index,
                                sym_Tss2_RC_Decode(rc));

        return 0;
#else /* HAVE_OPENSSL */
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}
#endif

char *tpm2_pcr_mask_to_string(uint32_t mask) {
        _cleanup_free_ char *s = NULL;

        FOREACH_PCR_IN_MASK(n, mask)
                if (strextendf_with_separator(&s, "+", "%d", n) < 0)
                        return NULL;

        if (!s)
                return strdup("");

        return TAKE_PTR(s);
}

int tpm2_pcr_mask_from_string(const char *arg, uint32_t *ret_mask) {
        uint32_t mask = 0;
        int r;

        assert(arg);
        assert(ret_mask);

        if (isempty(arg)) {
                *ret_mask = 0;
                return 0;
        }

        /* Parses a "," or "+" separated list of PCR indexes. We support "," since this is a list after all,
         * and most other tools expect comma separated PCR specifications. We also support "+" since in
         * /etc/crypttab the "," is already used to separate options, hence a different separator is nice to
         * avoid escaping. */

        const char *p = arg;
        for (;;) {
                _cleanup_free_ char *pcr = NULL;
                unsigned n;

                r = extract_first_word(&p, &pcr, ",+", EXTRACT_DONT_COALESCE_SEPARATORS);
                if (r == 0)
                        break;
                if (r < 0)
                        return log_error_errno(r, "Failed to parse PCR list: %s", arg);

                r = pcr_index_from_string(pcr);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse specified PCR or specified PCR is out of range: %s", pcr);
                n = r;
                SET_BIT(mask, n);
        }

        *ret_mask = mask;
        return 0;
}

int tpm2_make_pcr_json_array(uint32_t pcr_mask, JsonVariant **ret) {
        _cleanup_(json_variant_unrefp) JsonVariant *a = NULL;
        int r;

        assert(ret);

        for (size_t i = 0; i < TPM2_PCRS_MAX; i++) {
                _cleanup_(json_variant_unrefp) JsonVariant *e = NULL;

                if ((pcr_mask & (UINT32_C(1) << i)) == 0)
                        continue;

                r = json_variant_new_integer(&e, i);
                if (r < 0)
                        return r;

                r = json_variant_append_array(&a, e);
                if (r < 0)
                        return r;
        }

        if (!a)
                return json_variant_new_array(ret, NULL, 0);

        *ret = TAKE_PTR(a);
        return 0;
}

int tpm2_parse_pcr_json_array(JsonVariant *v, uint32_t *ret) {
        JsonVariant *e;
        uint32_t mask = 0;

        if (!json_variant_is_array(v))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PCR array is not a JSON array.");

        JSON_VARIANT_ARRAY_FOREACH(e, v) {
                uint64_t u;

                if (!json_variant_is_unsigned(e))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PCR is not an unsigned integer.");

                u = json_variant_unsigned(e);
                if (u >= TPM2_PCRS_MAX)
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PCR number out of range: %" PRIu64, u);

                mask |= UINT32_C(1) << u;
        }

        if (ret)
                *ret = mask;

        return 0;
}

int tpm2_make_luks2_json(
                int keyslot,
                uint32_t hash_pcr_mask,
                uint16_t pcr_bank,
                const void *pubkey,
                size_t pubkey_size,
                uint32_t pubkey_pcr_mask,
                uint16_t primary_alg,
                const void *blob,
                size_t blob_size,
                const void *policy_hash,
                size_t policy_hash_size,
                const void *salt,
                size_t salt_size,
                const void *srk_buf,
                size_t srk_buf_size,
                TPM2Flags flags,
                JsonVariant **ret) {

        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL, *hmj = NULL, *pkmj = NULL;
        _cleanup_free_ char *keyslot_as_string = NULL;
        int r;

        assert(blob || blob_size == 0);
        assert(policy_hash || policy_hash_size == 0);
        assert(pubkey || pubkey_size == 0);

        if (asprintf(&keyslot_as_string, "%i", keyslot) < 0)
                return -ENOMEM;

        r = tpm2_make_pcr_json_array(hash_pcr_mask, &hmj);
        if (r < 0)
                return r;

        if (pubkey_pcr_mask != 0) {
                r = tpm2_make_pcr_json_array(pubkey_pcr_mask, &pkmj);
                if (r < 0)
                        return r;
        }

        /* Note: We made the mistake of using "-" in the field names, which isn't particular compatible with
         * other programming languages. Let's not make things worse though, i.e. future additions to the JSON
         * object should use "_" rather than "-" in field names. */

        r = json_build(&v,
                       JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("type", JSON_BUILD_CONST_STRING("systemd-tpm2")),
                                       JSON_BUILD_PAIR("keyslots", JSON_BUILD_ARRAY(JSON_BUILD_STRING(keyslot_as_string))),
                                       JSON_BUILD_PAIR("tpm2-blob", JSON_BUILD_BASE64(blob, blob_size)),
                                       JSON_BUILD_PAIR("tpm2-pcrs", JSON_BUILD_VARIANT(hmj)),
                                       JSON_BUILD_PAIR_CONDITION(!!tpm2_hash_alg_to_string(pcr_bank), "tpm2-pcr-bank", JSON_BUILD_STRING(tpm2_hash_alg_to_string(pcr_bank))),
                                       JSON_BUILD_PAIR_CONDITION(!!tpm2_asym_alg_to_string(primary_alg), "tpm2-primary-alg", JSON_BUILD_STRING(tpm2_asym_alg_to_string(primary_alg))),
                                       JSON_BUILD_PAIR("tpm2-policy-hash", JSON_BUILD_HEX(policy_hash, policy_hash_size)),
                                       JSON_BUILD_PAIR("tpm2-pin", JSON_BUILD_BOOLEAN(flags & TPM2_FLAGS_USE_PIN)),
                                       JSON_BUILD_PAIR_CONDITION(pubkey_pcr_mask != 0, "tpm2_pubkey_pcrs", JSON_BUILD_VARIANT(pkmj)),
                                       JSON_BUILD_PAIR_CONDITION(pubkey_pcr_mask != 0, "tpm2_pubkey", JSON_BUILD_BASE64(pubkey, pubkey_size)),
                                       JSON_BUILD_PAIR_CONDITION(salt, "tpm2_salt", JSON_BUILD_BASE64(salt, salt_size)),
                                       JSON_BUILD_PAIR_CONDITION(srk_buf, "tpm2_srk", JSON_BUILD_BASE64(srk_buf, srk_buf_size))));
        if (r < 0)
                return r;

        if (ret)
                *ret = TAKE_PTR(v);

        return keyslot;
}

int tpm2_parse_luks2_json(
                JsonVariant *v,
                int *ret_keyslot,
                uint32_t *ret_hash_pcr_mask,
                uint16_t *ret_pcr_bank,
                void **ret_pubkey,
                size_t *ret_pubkey_size,
                uint32_t *ret_pubkey_pcr_mask,
                uint16_t *ret_primary_alg,
                void **ret_blob,
                size_t *ret_blob_size,
                void **ret_policy_hash,
                size_t *ret_policy_hash_size,
                void **ret_salt,
                size_t *ret_salt_size,
                void **ret_srk_buf,
                size_t *ret_srk_buf_size,
                TPM2Flags *ret_flags) {

        _cleanup_free_ void *blob = NULL, *policy_hash = NULL, *pubkey = NULL, *salt = NULL, *srk_buf = NULL;
        size_t blob_size = 0, policy_hash_size = 0, pubkey_size = 0, salt_size = 0, srk_buf_size = 0;
        uint32_t hash_pcr_mask = 0, pubkey_pcr_mask = 0;
        uint16_t primary_alg = TPM2_ALG_ECC; /* ECC was the only supported algorithm in systemd < 250, use that as implied default, for compatibility */
        uint16_t pcr_bank = UINT16_MAX; /* default: pick automatically */
        int r, keyslot = -1;
        TPM2Flags flags = 0;
        JsonVariant *w;

        assert(v);

        if (ret_keyslot) {
                keyslot = cryptsetup_get_keyslot_from_token(v);
                if (keyslot < 0) {
                        /* Return a recognizable error when parsing this field, so that callers can handle parsing
                         * errors of the keyslots field gracefully, since it's not 'owned' by us, but by the LUKS2
                         * spec */
                        log_debug_errno(keyslot, "Failed to extract keyslot index from TPM2 JSON data token, skipping: %m");
                        return -EUCLEAN;
                }
        }

        w = json_variant_by_key(v, "tpm2-pcrs");
        if (!w)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 token data lacks 'tpm2-pcrs' field.");

        r = tpm2_parse_pcr_json_array(w, &hash_pcr_mask);
        if (r < 0)
                return log_debug_errno(r, "Failed to parse TPM2 PCR mask: %m");

        /* The bank field is optional, since it was added in systemd 250 only. Before the bank was hardcoded
         * to SHA256. */
        w = json_variant_by_key(v, "tpm2-pcr-bank");
        if (w) {
                /* The PCR bank field is optional */

                if (!json_variant_is_string(w))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PCR bank is not a string.");

                r = tpm2_hash_alg_from_string(json_variant_string(w));
                if (r < 0)
                        return log_debug_errno(r, "TPM2 PCR bank invalid or not supported: %s", json_variant_string(w));

                pcr_bank = r;
        }

        /* The primary key algorithm field is optional, since it was also added in systemd 250 only. Before
         * the algorithm was hardcoded to ECC. */
        w = json_variant_by_key(v, "tpm2-primary-alg");
        if (w) {
                /* The primary key algorithm is optional */

                if (!json_variant_is_string(w))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 primary key algorithm is not a string.");

                r = tpm2_asym_alg_from_string(json_variant_string(w));
                if (r < 0)
                        return log_debug_errno(r, "TPM2 asymmetric algorithm invalid or not supported: %s", json_variant_string(w));

                primary_alg = r;
        }

        w = json_variant_by_key(v, "tpm2-blob");
        if (!w)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 token data lacks 'tpm2-blob' field.");

        r = json_variant_unbase64(w, &blob, &blob_size);
        if (r < 0)
                return log_debug_errno(r, "Invalid base64 data in 'tpm2-blob' field.");

        w = json_variant_by_key(v, "tpm2-policy-hash");
        if (!w)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 token data lacks 'tpm2-policy-hash' field.");

        r = json_variant_unhex(w, &policy_hash, &policy_hash_size);
        if (r < 0)
                return log_debug_errno(r, "Invalid base64 data in 'tpm2-policy-hash' field.");

        w = json_variant_by_key(v, "tpm2-pin");
        if (w) {
                if (!json_variant_is_boolean(w))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PIN policy is not a boolean.");

                SET_FLAG(flags, TPM2_FLAGS_USE_PIN, json_variant_boolean(w));
        }

        w = json_variant_by_key(v, "tpm2_salt");
        if (w) {
                r = json_variant_unbase64(w, &salt, &salt_size);
                if (r < 0)
                        return log_debug_errno(r, "Invalid base64 data in 'tpm2_salt' field.");
        }

        w = json_variant_by_key(v, "tpm2_pubkey_pcrs");
        if (w) {
                r = tpm2_parse_pcr_json_array(w, &pubkey_pcr_mask);
                if (r < 0)
                        return r;
        }

        w = json_variant_by_key(v, "tpm2_pubkey");
        if (w) {
                r = json_variant_unbase64(w, &pubkey, &pubkey_size);
                if (r < 0)
                        return log_debug_errno(r, "Failed to decode PCR public key.");
        } else if (pubkey_pcr_mask != 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Public key PCR mask set, but not public key included in JSON data, refusing.");

        w = json_variant_by_key(v, "tpm2_srk");
        if (w) {
                r = json_variant_unbase64(w, &srk_buf, &srk_buf_size);
                if (r < 0)
                        return log_debug_errno(r, "Invalid base64 data in 'tpm2_srk' field.");
        }

        if (ret_keyslot)
                *ret_keyslot = keyslot;
        if (ret_hash_pcr_mask)
                *ret_hash_pcr_mask = hash_pcr_mask;
        if (ret_pcr_bank)
                *ret_pcr_bank = pcr_bank;
        if (ret_pubkey)
                *ret_pubkey = TAKE_PTR(pubkey);
        if (ret_pubkey_size)
                *ret_pubkey_size = pubkey_size;
        if (ret_pubkey_pcr_mask)
                *ret_pubkey_pcr_mask = pubkey_pcr_mask;
        if (ret_primary_alg)
                *ret_primary_alg = primary_alg;
        if (ret_blob)
                *ret_blob = TAKE_PTR(blob);
        if (ret_blob_size)
                *ret_blob_size = blob_size;
        if (ret_policy_hash)
                *ret_policy_hash = TAKE_PTR(policy_hash);
        if (ret_policy_hash_size)
                *ret_policy_hash_size = policy_hash_size;
        if (ret_salt)
                *ret_salt = TAKE_PTR(salt);
        if (ret_salt_size)
                *ret_salt_size = salt_size;
        if (ret_flags)
                *ret_flags = flags;
        if (ret_srk_buf)
                *ret_srk_buf = TAKE_PTR(srk_buf);
        if (ret_srk_buf_size)
                *ret_srk_buf_size = srk_buf_size;

        return 0;
}

const char *tpm2_hash_alg_to_string(uint16_t alg) {
        if (alg == TPM2_ALG_SHA1)
                return "sha1";
        if (alg == TPM2_ALG_SHA256)
                return "sha256";
        if (alg == TPM2_ALG_SHA384)
                return "sha384";
        if (alg == TPM2_ALG_SHA512)
                return "sha512";
        return NULL;
}

int tpm2_hash_alg_from_string(const char *alg) {
        if (strcaseeq_ptr(alg, "sha1"))
                return TPM2_ALG_SHA1;
        if (strcaseeq_ptr(alg, "sha256"))
                return TPM2_ALG_SHA256;
        if (strcaseeq_ptr(alg, "sha384"))
                return TPM2_ALG_SHA384;
        if (strcaseeq_ptr(alg, "sha512"))
                return TPM2_ALG_SHA512;
        return -EINVAL;
}

const char *tpm2_asym_alg_to_string(uint16_t alg) {
        if (alg == TPM2_ALG_ECC)
                return "ecc";
        if (alg == TPM2_ALG_RSA)
                return "rsa";
        return NULL;
}

int tpm2_asym_alg_from_string(const char *alg) {
        if (strcaseeq_ptr(alg, "ecc"))
                return TPM2_ALG_ECC;
        if (strcaseeq_ptr(alg, "rsa"))
                return TPM2_ALG_RSA;
        return -EINVAL;
}

Tpm2Support tpm2_support(void) {
        Tpm2Support support = TPM2_SUPPORT_NONE;
        int r;

        if (detect_container() <= 0) {
                /* Check if there's a /dev/tpmrm* device via sysfs. If we run in a container we likely just
                 * got the host sysfs mounted. Since devices are generally not virtualized for containers,
                 * let's assume containers never have a TPM, at least for now. */

                r = dir_is_empty("/sys/class/tpmrm", /* ignore_hidden_or_backup= */ false);
                if (r < 0) {
                        if (r != -ENOENT)
                                log_debug_errno(r, "Unable to test whether /sys/class/tpmrm/ exists and is populated, assuming it is not: %m");
                } else if (r == 0) /* populated! */
                        support |= TPM2_SUPPORT_SUBSYSTEM|TPM2_SUPPORT_DRIVER;
                else
                        /* If the directory exists but is empty, we know the subsystem is enabled but no
                         * driver has been loaded yet. */
                        support |= TPM2_SUPPORT_SUBSYSTEM;
        }

        if (efi_has_tpm2())
                support |= TPM2_SUPPORT_FIRMWARE;

#if HAVE_TPM2
        support |= TPM2_SUPPORT_SYSTEM;

        r = dlopen_tpm2();
        if (r >= 0)
                support |= TPM2_SUPPORT_LIBRARIES;
#endif

        return support;
}

int tpm2_parse_pcr_argument(const char *arg, uint32_t *mask) {
        uint32_t m;
        int r;

        assert(mask);

        /* For use in getopt_long() command line parsers: merges masks specified on the command line */

        if (isempty(arg)) {
                *mask = 0;
                return 0;
        }

        r = tpm2_pcr_mask_from_string(arg, &m);
        if (r < 0)
                return r;

        if (*mask == UINT32_MAX)
                *mask = m;
        else
                *mask |= m;

        return 0;
}

int tpm2_load_pcr_signature(const char *path, JsonVariant **ret) {
        _cleanup_strv_free_ char **search = NULL;
        _cleanup_free_ char *discovered_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        /* Tries to load a JSON PCR signature file. Takes an absolute path, a simple file name or NULL. In
         * the latter two cases searches in /etc/, /usr/lib/, /run/, as usual. */

        search = strv_split_nulstr(CONF_PATHS_NULSTR("systemd"));
        if (!search)
                return log_oom();

        if (!path) {
                /* If no path is specified, then look for "tpm2-pcr-signature.json" automatically. Also, in
                 * this case include /.extra/ in the search path, but only in this case, and if we run in the
                 * initrd. We don't want to be too eager here, after all /.extra/ is untrusted territory. */

                path = "tpm2-pcr-signature.json";

                if (in_initrd())
                        if (strv_extend(&search, "/.extra") < 0)
                                return log_oom();
        }

        r = search_and_fopen(path, "re", NULL, (const char**) search, &f, &discovered_path);
        if (r < 0)
                return log_debug_errno(r, "Failed to find TPM PCR signature file '%s': %m", path);

        r = json_parse_file(f, discovered_path, 0, ret, NULL, NULL);
        if (r < 0)
                return log_debug_errno(r, "Failed to parse TPM PCR signature JSON object '%s': %m", discovered_path);

        return 0;
}

int tpm2_load_pcr_public_key(const char *path, void **ret_pubkey, size_t *ret_pubkey_size) {
        _cleanup_free_ char *discovered_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        /* Tries to load a PCR public key file. Takes an absolute path, a simple file name or NULL. In the
         * latter two cases searches in /etc/, /usr/lib/, /run/, as usual. */

        if (!path)
                path = "tpm2-pcr-public-key.pem";

        r = search_and_fopen(path, "re", NULL, (const char**) CONF_PATHS_STRV("systemd"), &f, &discovered_path);
        if (r < 0)
                return log_debug_errno(r, "Failed to find TPM PCR public key file '%s': %m", path);

        r = read_full_stream(f, (char**) ret_pubkey, ret_pubkey_size);
        if (r < 0)
                return log_debug_errno(r, "Failed to load TPM PCR public key PEM file '%s': %m", discovered_path);

        return 0;
}

#define PBKDF2_HMAC_SHA256_ITERATIONS 10000

/*
 * Implements PBKDF2 HMAC SHA256 for a derived keylen of 32
 * bytes and for PBKDF2_HMAC_SHA256_ITERATIONS count.
 * I found the wikipedia entry relevant and it contains links to
 * relevant RFCs:
 *   - https://en.wikipedia.org/wiki/PBKDF2
 *   - https://www.rfc-editor.org/rfc/rfc2898#section-5.2
 */
int tpm2_util_pbkdf2_hmac_sha256(const void *pass,
                    size_t passlen,
                    const void *salt,
                    size_t saltlen,
                    uint8_t ret_key[static SHA256_DIGEST_SIZE]) {

        uint8_t _cleanup_(erase_and_freep) *buffer = NULL;
        uint8_t u[SHA256_DIGEST_SIZE];

        /* To keep this simple, since derived KeyLen (dkLen in docs)
         * Is the same as the hash output, we don't need multiple
         * blocks. Part of the algorithm is to add the block count
         * in, but this can be hardcoded to 1.
         */
        static const uint8_t block_cnt[] = { 0, 0, 0, 1 };

        assert (salt);
        assert (saltlen > 0);
        assert (saltlen <= (SIZE_MAX - sizeof(block_cnt)));
        assert (passlen > 0);

        /*
         * Build a buffer of salt + block_cnt and hmac_sha256 it we
         * do this as we don't have a context builder for HMAC_SHA256.
         */
        buffer = malloc(saltlen + sizeof(block_cnt));
        if (!buffer)
                return -ENOMEM;

        memcpy(buffer, salt, saltlen);
        memcpy(&buffer[saltlen], block_cnt, sizeof(block_cnt));

        hmac_sha256(pass, passlen, buffer, saltlen + sizeof(block_cnt), u);

        /* dk needs to be an unmodified u as u gets modified in the loop */
        memcpy(ret_key, u, SHA256_DIGEST_SIZE);
        uint8_t *dk = ret_key;

        for (size_t i = 1; i < PBKDF2_HMAC_SHA256_ITERATIONS; i++) {
                hmac_sha256(pass, passlen, u, sizeof(u), u);

                for (size_t j=0; j < sizeof(u); j++)
                        dk[j] ^= u[j];
        }

        return 0;
}

static const char* const pcr_index_table[_PCR_INDEX_MAX_DEFINED] = {
        [PCR_PLATFORM_CODE]       = "platform-code",
        [PCR_PLATFORM_CONFIG]     = "platform-config",
        [PCR_EXTERNAL_CODE]       = "external-code",
        [PCR_EXTERNAL_CONFIG]     = "external-config",
        [PCR_BOOT_LOADER_CODE]    = "boot-loader-code",
        [PCR_BOOT_LOADER_CONFIG]  = "boot-loader-config",
        [PCR_HOST_PLATFORM]       = "host-platform",
        [PCR_SECURE_BOOT_POLICY]  = "secure-boot-policy",
        [PCR_KERNEL_INITRD]       = "kernel-initrd",
        [PCR_IMA]                 = "ima",
        [PCR_KERNEL_BOOT]         = "kernel-boot",
        [PCR_KERNEL_CONFIG]       = "kernel-config",
        [PCR_SYSEXTS]             = "sysexts",
        [PCR_SHIM_POLICY]         = "shim-policy",
        [PCR_SYSTEM_IDENTITY]     = "system-identity",
        [PCR_DEBUG]               = "debug",
        [PCR_APPLICATION_SUPPORT] = "application-support",
};

DEFINE_STRING_TABLE_LOOKUP_FROM_STRING_WITH_FALLBACK(pcr_index, int, TPM2_PCRS_MAX - 1);
DEFINE_STRING_TABLE_LOOKUP_TO_STRING(pcr_index, int);
