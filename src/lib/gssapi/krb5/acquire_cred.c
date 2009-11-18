/* -*- mode: c; indent-tabs-mode: nil -*- */
/*
 * Copyright 2000, 2007, 2008 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 */
/*
 * Copyright 1993 by OpenVision Technologies, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appears in all copies and
 * that both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of OpenVision not be used
 * in advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission. OpenVision makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * OPENVISION DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL OPENVISION BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright (C) 1998 by the FundsXpress, INC.
 *
 * All rights reserved.
 *
 * Export of this software from the United States of America may require
 * a specific license from the United States Government.  It is the
 * responsibility of any person or organization contemplating export to
 * obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of FundsXpress. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  FundsXpress makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "k5-int.h"
#include "gssapiP_krb5.h"
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#if defined(USE_KIM)
#include <kim/kim.h>
#include "kim_library_private.h"
#elif defined(USE_LEASH)
#ifdef _WIN64
#define LEASH_DLL "leashw64.dll"
#else
#define LEASH_DLL "leashw32.dll"
#endif
static void (*pLeash_AcquireInitialTicketsIfNeeded)(krb5_context,krb5_principal,char*,int) = NULL;
static HANDLE hLeashDLL = INVALID_HANDLE_VALUE;
#endif

#ifndef LEAN_CLIENT
k5_mutex_t gssint_krb5_keytab_lock = K5_MUTEX_PARTIAL_INITIALIZER;
static char *krb5_gss_keytab = NULL;

/* Heimdal calls this gsskrb5_register_acceptor_identity. */
OM_uint32
gss_krb5int_register_acceptor_identity(OM_uint32 *minor_status,
                                       const gss_OID desired_mech,
                                       const gss_OID desired_object,
                                       gss_buffer_t value)
{
    char *new, *old;
    int err;

    err = gss_krb5int_initialize_library();
    if (err != 0)
        return GSS_S_FAILURE;

    if (value->value == NULL)
        return GSS_S_FAILURE;

    new = strdup((char *)value->value);
    if (new == NULL)
        return GSS_S_FAILURE;

    err = k5_mutex_lock(&gssint_krb5_keytab_lock);
    if (err) {
        free(new);
        return GSS_S_FAILURE;
    }
    old = krb5_gss_keytab;
    krb5_gss_keytab = new;
    k5_mutex_unlock(&gssint_krb5_keytab_lock);
    if (old != NULL)
        free(old);
    return GSS_S_COMPLETE;
}

/* get credentials corresponding to a key in the krb5 keytab.
   If the default name is requested, return the name in output_name.
   If output_name is non-NULL, the caller will use or free it, regardless
   of the return value.
   If successful, set the keytab-specific fields in cred
*/

static OM_uint32
acquire_accept_cred(krb5_context context,
                    OM_uint32 *minor_status,
                    krb5_gss_name_t desired_name,
                    krb5_gss_name_t *output_name,
                    krb5_gss_cred_id_rec *cred)
{
    krb5_error_code code;
    krb5_principal princ;
    krb5_keytab kt;
    krb5_keytab_entry entry;

    *output_name = NULL;
    cred->keytab = NULL;

    /* open the default keytab */

    code = gss_krb5int_initialize_library();
    if (code != 0) {
        *minor_status = code;
        return GSS_S_FAILURE;
    }
    code = k5_mutex_lock(&gssint_krb5_keytab_lock);
    if (code) {
        *minor_status = code;
        return GSS_S_FAILURE;
    }
    if (krb5_gss_keytab != NULL) {
        code = krb5_kt_resolve(context, krb5_gss_keytab, &kt);
        k5_mutex_unlock(&gssint_krb5_keytab_lock);
    } else {
        k5_mutex_unlock(&gssint_krb5_keytab_lock);
        code = krb5_kt_default(context, &kt);
    }

    if (code) {
        *minor_status = code;
        return(GSS_S_CRED_UNAVAIL);
    }

    if (desired_name != NULL) {
        princ = desired_name->princ;
        if ((code = krb5_kt_get_entry(context, kt, princ, 0, 0, &entry))) {
            (void) krb5_kt_close(context, kt);
            if (code == KRB5_KT_NOTFOUND) {
                char *errstr = (char *)krb5_get_error_message(context, code);
                krb5_set_error_message(context, KG_KEYTAB_NOMATCH, "%s", errstr);
                krb5_free_error_message(context, errstr);
                *minor_status = KG_KEYTAB_NOMATCH;
            } else
                *minor_status = code;
            return(GSS_S_CRED_UNAVAIL);
        }
        krb5_kt_free_entry(context, &entry);

        /* Open the replay cache for this principal. */
        if ((code = krb5_get_server_rcache(context,
                                           krb5_princ_component(context, princ, 0),
                                           &cred->rcache))) {
            *minor_status = code;
            return(GSS_S_FAILURE);
        }

    }

/* hooray.  we made it */

    cred->keytab = kt;

    return(GSS_S_COMPLETE);
}
#endif /* LEAN_CLIENT */

/* get credentials corresponding to the default credential cache.
   If the default name is requested, return the name in output_name.
   If output_name is non-NULL, the caller will use or free it, regardless
   of the return value.
   If successful, set the ccache-specific fields in cred.
*/

static OM_uint32
acquire_init_cred(krb5_context context,
                  OM_uint32 *minor_status,
                  krb5_gss_name_t desired_name,
                  krb5_gss_name_t *output_name,
                  gss_buffer_t password,
                  krb5_gss_cred_id_rec *cred)
{
    krb5_error_code code;
    krb5_ccache ccache;
    krb5_principal princ = NULL, tmp_princ;
    krb5_cc_cursor cur;
    krb5_creds creds;
    int got_endtime;
    int caller_provided_ccache_name = 0;
    krb5_data password_data;

    cred->ccache = NULL;

    /* load the GSS ccache name into the kg_context */

    if (GSS_ERROR(kg_sync_ccache_name(context, minor_status)))
        return(GSS_S_FAILURE);

    /* check to see if the caller provided a ccache name if so
     * we will just use that and not search the cache collection */
    if (GSS_ERROR(kg_caller_provided_ccache_name (minor_status, &caller_provided_ccache_name))) {
        return(GSS_S_FAILURE);
    }

#if defined(USE_KIM) || defined(USE_LEASH)
    if (desired_name && !caller_provided_ccache_name) {
#if defined(USE_KIM)
        kim_error err = KIM_NO_ERROR;
        kim_ccache kimccache = NULL;
        kim_identity identity = NULL;
        kim_credential_state state;

        err = kim_identity_create_from_krb5_principal (&identity,
                                                       context,
                                                       desired_name->princ);

        if (!err) {
            err = kim_ccache_create_from_client_identity (&kimccache, identity);
        }

        if (!err) {
            err = kim_ccache_get_state (kimccache, &state);
        }

        if (!err && state != kim_credentials_state_valid) {
            if (state == kim_credentials_state_needs_validation) {
                err = kim_ccache_validate (kimccache, KIM_OPTIONS_DEFAULT);
            } else {
                kim_ccache_free (&kimccache);
                ccache = NULL;
            }
        }

        if (!kimccache && kim_library_allow_automatic_prompting ()) {
            /* ccache does not already exist, create a new one */
            err = kim_ccache_create_new (&kimccache, identity,
                                         KIM_OPTIONS_DEFAULT);
        }

        if (!err) {
            err = kim_ccache_get_krb5_ccache (kimccache, context, &ccache);
        }

        kim_ccache_free (&kimccache);
        kim_identity_free (&identity);

        if (err) {
            *minor_status = err;
            return(GSS_S_CRED_UNAVAIL);
        }

#elif defined(USE_LEASH)
        if ( hLeashDLL == INVALID_HANDLE_VALUE ) {
            hLeashDLL = LoadLibrary(LEASH_DLL);
            if ( hLeashDLL != INVALID_HANDLE_VALUE ) {
                (FARPROC) pLeash_AcquireInitialTicketsIfNeeded =
                    GetProcAddress(hLeashDLL, "not_an_API_Leash_AcquireInitialTicketsIfNeeded");
            }
        }

        if ( pLeash_AcquireInitialTicketsIfNeeded ) {
            char ccname[256]="";
            pLeash_AcquireInitialTicketsIfNeeded(context, desired_name->princ, ccname, sizeof(ccname));
            if (!ccname[0]) {
                *minor_status = KRB5_CC_NOTFOUND;
                return(GSS_S_CRED_UNAVAIL);
            }

            if ((code = krb5_cc_resolve (context, ccname, &ccache))) {
                *minor_status = code;
                return(GSS_S_CRED_UNAVAIL);
            }
        } else {
            /* leash dll not available, open the default credential cache */

            if ((code = krb5int_cc_default(context, &ccache))) {
                *minor_status = code;
                return(GSS_S_CRED_UNAVAIL);
            }
        }
#endif /* USE_LEASH */
    } else
#endif /* USE_KIM || USE_LEASH */
    {
        /* open the default credential cache */

        if ((code = krb5int_cc_default(context, &ccache))) {
            *minor_status = code;
            return(GSS_S_CRED_UNAVAIL);
        }
    }

    if (password != GSS_C_NO_BUFFER) {
        password_data.length = password->length;
        password_data.data = (char *)password->value;
    }

    /* turn off OPENCLOSE mode while extensive frobbing is going on */
    code = krb5_cc_set_flags(context, ccache, 0);
    if (code == KRB5_FCC_NOFILE &&
        password != GSS_C_NO_BUFFER && desired_name != NULL) {
        /* Well, we can create a memory ccache. */
        code = krb5_cc_new_unique(context, "MEMORY", NULL, &ccache);
        if (code == 0)
            code = krb5_cc_initialize(context, ccache, desired_name->princ);
    }
    if (code != 0) {
        (void)krb5_cc_close(context, ccache);
        *minor_status = code;
        return(GSS_S_CRED_UNAVAIL);
    }

    /* get out the principal name and see if it matches */
    code = krb5_cc_get_principal(context, ccache, &princ);
    if (code != 0) {
        (void)krb5_cc_close(context, ccache);
        *minor_status = code;
        return(GSS_S_FAILURE);
    }

    if (desired_name != (krb5_gss_name_t)NULL) {
        if (!krb5_principal_compare(context, princ, desired_name->princ)) {
            (void)krb5_free_principal(context, princ);
            (void)krb5_cc_close(context, ccache);
            *minor_status = KG_CCACHE_NOMATCH;
            return(GSS_S_CRED_UNAVAIL);
        }
        (void)krb5_free_principal(context, princ);
        princ = desired_name->princ;
    } else {
        if ((code = kg_init_name(context, princ, NULL,
                                 KG_INIT_NAME_NO_COPY | KG_INIT_NAME_INTERN,
                                 output_name))) {
            (void)krb5_free_principal(context, princ);
            (void)krb5_cc_close(context, ccache);
            *minor_status = code;
            return(GSS_S_FAILURE);
        }
    }

    if (cred->iakerb_mech) {
        assert(password != GSS_C_NO_BUFFER);

        /* stash the password for later */
        code = krb5int_copy_data_contents_add0(context, &password_data,
                                               &cred->password);
        if (code != 0) {
            *minor_status = code;
            return GSS_S_FAILURE;
        }

        /* restore the OPENCLOSE flag */
        code = krb5_cc_set_flags(context, ccache, KRB5_TC_OPENCLOSE);
        if (code != 0) {
            *minor_status = code;
            return GSS_S_FAILURE;
        }

        cred->ccache = ccache;
        return GSS_S_COMPLETE;
    }

    /* iterate over the ccache, find the tgt */

    if ((code = krb5_cc_start_seq_get(context, ccache, &cur))) {
        (void)krb5_cc_close(context, ccache);
        *minor_status = code;
        return(GSS_S_FAILURE);
    }

    /* this is hairy.  If there's a tgt for the principal's local realm
       in here, that's what we want for the expire time.  But if
       there's not, then we want to use the first key.  */

    got_endtime = 0;

    code = krb5_build_principal_ext(context, &tmp_princ,
                                    krb5_princ_realm(context, princ)->length,
                                    krb5_princ_realm(context, princ)->data,
                                    6, "krbtgt",
                                    krb5_princ_realm(context, princ)->length,
                                    krb5_princ_realm(context, princ)->data,
                                    0);
    if (code) {
        (void)krb5_cc_close(context, ccache);
        *minor_status = code;
        return(GSS_S_FAILURE);
    }
    while (!(code = krb5_cc_next_cred(context, ccache, &cur, &creds))) {
        if (krb5_principal_compare(context, tmp_princ, creds.server)) {
            cred->tgt_expire = creds.times.endtime;
            got_endtime = 1;
            *minor_status = 0;
            code = 0;
            krb5_free_cred_contents(context, &creds);
            break;
        }
        if (got_endtime == 0) {
            cred->tgt_expire = creds.times.endtime;
            got_endtime = 1;
        }
        krb5_free_cred_contents(context, &creds);
    }
    krb5_free_principal(context, tmp_princ);

    if (code == KRB5_CC_END && !got_endtime && password != GSS_C_NO_BUFFER) {
        krb5_error_code code2;
        krb5_data password_data0;

        code2 = krb5int_copy_data_contents_add0(context, &password_data,
                                                &password_data0);
        if (code2 == 0) {
            code2 = krb5_get_init_creds_password(context,
                                                 &creds,
                                                 princ,
                                                 password_data0.data,
                                                 NULL,
                                                 NULL,
                                                 0,
                                                 NULL,
                                                 NULL);
            zap(password_data0.data, password_data0.length);
            krb5_free_data_contents(context, &password_data0);
        }
        if (code2 == 0) {
            code2 = krb5_cc_store_cred(context, ccache, &creds);
            if (code2 == 0) {
                cred->tgt_expire = creds.times.endtime;
                got_endtime = 1;
                code = 0;
            }
            krb5_free_cred_contents(context, &creds);
        }
    }

    if (code && code != KRB5_CC_END) {
        /* this means some error occurred reading the ccache */
        (void)krb5_cc_end_seq_get(context, ccache, &cur);
        (void)krb5_cc_close(context, ccache);
        *minor_status = code;
        return(GSS_S_FAILURE);
    } else if (! got_endtime) {
        /* this means the ccache was entirely empty */
        (void)krb5_cc_end_seq_get(context, ccache, &cur);
        (void)krb5_cc_close(context, ccache);
        *minor_status = KG_EMPTY_CCACHE;
        return(GSS_S_FAILURE);
    } else {
        /* this means that we found an endtime to use. */
        if ((code = krb5_cc_end_seq_get(context, ccache, &cur))) {
            (void)krb5_cc_close(context, ccache);
            *minor_status = code;
            return(GSS_S_FAILURE);
        }
        if ((code = krb5_cc_set_flags(context, ccache, KRB5_TC_OPENCLOSE))) {
            (void)krb5_cc_close(context, ccache);
            *minor_status = code;
            return(GSS_S_FAILURE);
        }
    }

    /* the credentials match and are valid */

    cred->ccache = ccache;
    /* minor_status is set while we are iterating over the ccache */
    return(GSS_S_COMPLETE);
}

/*ARGSUSED*/
static OM_uint32
acquire_cred(minor_status, desired_name, password, time_req,
             desired_mechs, cred_usage, output_cred_handle,
             actual_mechs, time_rec, req_iakerb)
    OM_uint32 *minor_status;
    const gss_name_t desired_name;
    const gss_buffer_t password;
    OM_uint32 time_req;
    const gss_OID_set desired_mechs;
    gss_cred_usage_t cred_usage;
    gss_cred_id_t *output_cred_handle;
    gss_OID_set *actual_mechs;
    OM_uint32 *time_rec;
    int req_iakerb;
{
    krb5_context context = NULL;
    size_t i;
    krb5_gss_cred_id_t cred = NULL;
    gss_OID_set ret_mechs = GSS_C_NO_OID_SET;
    int req_old, req_new;
    OM_uint32 ret;
    krb5_error_code code = 0;

    code = gss_krb5int_initialize_library();
    if (code)
        goto krb_error_out;

    code = krb5_gss_init_context(&context);
    if (code)
        goto krb_error_out;

    /* make sure all outputs are valid */

    *output_cred_handle = NULL;
    if (actual_mechs)
        *actual_mechs = NULL;
    if (time_rec)
        *time_rec = 0;

    /* validate the name */

    /*SUPPRESS 29*/
    if ((desired_name != GSS_C_NO_NAME) &&
        (! kg_validate_name(desired_name))) {
        code = G_VALIDATE_FAILED;
        goto krb_error_out;
    }

    if (req_iakerb &&
        (password == GSS_C_NO_BUFFER || cred_usage == GSS_C_BOTH)) {
        code = G_BAD_USAGE;
        goto krb_error_out;
    }

    /* verify that the requested mechanism set is the default, or
       contains krb5 */

    if (desired_mechs == GSS_C_NULL_OID_SET) {
        req_old = 1;
        req_new = 1;
    } else {
        req_old = 0;
        req_new = 0;

        for (i=0; i<desired_mechs->count; i++) {
            if (g_OID_equal(gss_mech_krb5_old, &(desired_mechs->elements[i])))
                req_old++;
            if (g_OID_equal(gss_mech_krb5, &(desired_mechs->elements[i])))
                req_new++;
        }

        if (!req_old && !req_new) {
            ret = GSS_S_BAD_MECH;
            goto error_out;
        }
    }

    /* create the gss cred structure */
    cred = k5alloc(sizeof(krb5_gss_cred_id_rec), &code);
    if (code != 0)
        goto krb_error_out;

    cred->usage = cred_usage;
    cred->name = NULL;
    cred->prerfc_mech = (req_old != 0) && (req_iakerb == 0);
    cred->rfc_mech = (req_new != 0) && (req_iakerb == 0);
    cred->iakerb_mech = req_iakerb;
    cred->default_identity = (desired_name == GSS_C_NO_NAME);

#ifndef LEAN_CLIENT
    cred->keytab = NULL;
#endif /* LEAN_CLIENT */
    cred->ccache = NULL;

    code = k5_mutex_init(&cred->lock);
    if (code)
        goto krb_error_out;

    /* Note that we don't need to lock this GSSAPI credential record
       here, because no other thread can gain access to it until we
       return it.  */

    if ((cred_usage != GSS_C_INITIATE) &&
        (cred_usage != GSS_C_ACCEPT) &&
        (cred_usage != GSS_C_BOTH)) {
        *minor_status = (OM_uint32) G_BAD_USAGE;
        goto error_out;
    }

    /* if requested, acquire credentials for accepting */
    /* this will fill in cred->name if the desired_name is not specified */
#ifndef LEAN_CLIENT
    if ((cred_usage == GSS_C_ACCEPT) ||
        (cred_usage == GSS_C_BOTH))
        if ((ret = acquire_accept_cred(context, minor_status,
                                       (krb5_gss_name_t)desired_name,
                                       &cred->name, cred))
            != GSS_S_COMPLETE) {
            goto error_out;
        }
#endif /* LEAN_CLIENT */

    /* if requested, acquire credentials for initiation */
    /* this will fill in cred->name if it wasn't set above, and
       the desired_name is not specified */

    if (cred_usage == GSS_C_INITIATE || cred_usage == GSS_C_BOTH) {
        ret = acquire_init_cred(context, minor_status,
                                cred->name ?
                                    cred->name : (krb5_gss_name_t)desired_name,
                                &cred->name, password, cred);
        if (ret != GSS_S_COMPLETE)
            goto error_out;
    }

    /* if the princ wasn't filled in already, fill it in now */

    if (!cred->name && (desired_name != GSS_C_NO_NAME)) {
        code = kg_duplicate_name(context,
                                 (krb5_gss_name_t)desired_name,
                                 0, &cred->name);
        if (code != 0)
            goto krb_error_out;
    }

    /*** at this point, the cred structure has been completely created */

    /* compute time_rec */

    if (cred_usage == GSS_C_ACCEPT) {
        if (time_rec)
            *time_rec = GSS_C_INDEFINITE;
    } else {
        krb5_timestamp now;

        code = krb5_timeofday(context, &now);
        if (code != 0)
            goto krb_error_out;

        if (time_rec)
            *time_rec = (cred->tgt_expire > now) ? (cred->tgt_expire - now) : 0;
    }

    /* create mechs */

    if (actual_mechs) {
        if (GSS_ERROR(ret = generic_gss_create_empty_oid_set(minor_status,
                                                             &ret_mechs)) ||
            (cred->prerfc_mech &&
             GSS_ERROR(ret = generic_gss_add_oid_set_member(minor_status,
                                                            gss_mech_krb5_old,
                                                            &ret_mechs))) ||
            (cred->rfc_mech &&
             GSS_ERROR(ret = generic_gss_add_oid_set_member(minor_status,
                                                            gss_mech_krb5,
                                                            &ret_mechs))) ||
            (cred->iakerb_mech &&
             GSS_ERROR(ret = generic_gss_add_oid_set_member(minor_status,
                                                            gss_mech_iakerb,
                                                            &ret_mechs)))) {
            goto error_out;
        }
    }

    /* intern the credential handle */

    if (! kg_save_cred_id((gss_cred_id_t) cred)) {
        *minor_status = (OM_uint32) G_VALIDATE_FAILED;
        ret = GSS_S_FAILURE;
        goto error_out;
    }

    /* return success */

    *minor_status = 0;
    *output_cred_handle = (gss_cred_id_t) cred;
    if (actual_mechs)
        *actual_mechs = ret_mechs;

    krb5_free_context(context);
    return(GSS_S_COMPLETE);

krb_error_out:
    *minor_status = code;
    ret = GSS_S_FAILURE;

error_out:
    if (ret_mechs != GSS_C_NO_OID_SET) {
        free(ret_mechs->elements);
        free(ret_mechs);
    }
    if (cred->ccache)
        (void)krb5_cc_close(context, cred->ccache);
#ifndef LEAN_CLIENT
    if (cred->keytab)
        (void)krb5_kt_close(context, cred->keytab);
#endif /* LEAN_CLIENT */
    if (cred->name)
        kg_release_name(context, 0, &cred->name);
    k5_mutex_destroy(&cred->lock);
    xfree(cred);
    save_error_info(*minor_status, context);
    krb5_free_context(context);
    return ret;
}

OM_uint32
gss_krb5int_set_cred_rcache(OM_uint32 *minor_status,
    gss_cred_id_t cred_handle,
    const gss_OID desired_oid,
    const gss_buffer_t value)
{
   krb5_gss_cred_id_t cred;
   krb5_error_code code;
   krb5_context context;
   krb5_rcache rcache;

   assert(value->length == sizeof(rcache));

   if (value->length != sizeof(rcache))
      return GSS_S_FAILURE;

   rcache = (krb5_rcache)value->value;

   if (cred_handle == GSS_C_NO_CREDENTIAL)
      return GSS_S_NO_CRED;

   cred = (krb5_gss_cred_id_t)cred_handle;

   code = krb5_gss_init_context(&context);
   if (code) {
       *minor_status = code;
       return GSS_S_FAILURE;
   }
   if (cred->rcache != NULL) {
      code = krb5_rc_close(context, cred->rcache);
      if (code) {
         *minor_status = code;
         krb5_free_context(context);
         return GSS_S_FAILURE;
      }
   }

   cred->rcache = rcache;

   krb5_free_context(context);

   *minor_status = 0;
   return GSS_S_COMPLETE;
}

OM_uint32
krb5_gss_acquire_cred(minor_status, desired_name, time_req,
                      desired_mechs, cred_usage, output_cred_handle,
                      actual_mechs, time_rec)
    OM_uint32 *minor_status;
    gss_name_t desired_name;
    OM_uint32 time_req;
    gss_OID_set desired_mechs;
    gss_cred_usage_t cred_usage;
    gss_cred_id_t *output_cred_handle;
    gss_OID_set *actual_mechs;
    OM_uint32 *time_rec;
{
    return acquire_cred(minor_status, desired_name, GSS_C_NO_BUFFER,
                        time_req, desired_mechs,
                        cred_usage, output_cred_handle, actual_mechs,
                        time_rec, 0);
}

OM_uint32
krb5_gss_acquire_cred_with_password(OM_uint32 *minor_status,
                                    const gss_name_t desired_name,
                                    const gss_buffer_t password,
                                    OM_uint32 time_req,
                                    const gss_OID_set desired_mechs,
                                    int cred_usage,
                                    gss_cred_id_t *output_cred_handle,
                                    gss_OID_set *actual_mechs,
                                    OM_uint32 *time_rec)
{
    return acquire_cred(minor_status, desired_name, password,
                        time_req, desired_mechs,
                        cred_usage, output_cred_handle, actual_mechs,
                        time_rec, 0);
}

OM_uint32
iakerb_gss_acquire_cred_with_password(OM_uint32 *minor_status,
                                      const gss_name_t desired_name,
                                      const gss_buffer_t password,
                                      OM_uint32 time_req,
                                      const gss_OID_set desired_mechs,
                                      int cred_usage,
                                      gss_cred_id_t *output_cred_handle,
                                      gss_OID_set *actual_mechs,
                                      OM_uint32 *time_rec)
{
    return acquire_cred(minor_status, desired_name, password,
                        time_req, desired_mechs,
                        cred_usage, output_cred_handle, actual_mechs,
                        time_rec, 1);
}

