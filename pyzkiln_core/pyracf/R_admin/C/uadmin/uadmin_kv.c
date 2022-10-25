//
// uadmin_kv.c - methods to build and manage key-value lists from the 
//             results of a RACF function (profile extract).
//
//             The results returned from RACF are EBCDIC byte strings.
//             They need to be trascoded to ASCII, and null-terminated
//             when placed in a key-value pair.
//
// Author: Joe Bostian
// Copyright Contributors to the Ambitus Project.
// SPDX-License-Identifier: Apache-2.0 
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <iconv.h>

#include "r_admin.h"
#include "uadmin.h"
#include "irrpcomp.h"
#include "keyval.h"
#include "transcode.h"
#include "bytes.h"

const iconv_t CD_NO_TRANSCODE  = (iconv_t)0x00000000;

// Local prototypes
RC uadmin_build_base_segment(BYTE *, BASE_SEGMENT_T *, LOGGER_T *);
int count_base_segment_fields(BASE_SEGMENT_T *);
RC uadmin_build_omvs_segment(BYTE *, BASE_SEGMENT_T *, LOGGER_T *);
int count_omvs_segment_fields(OMVS_SEGMENT_T *);
void build_segment_header(BYTE, char *, int);
RC add_key_value_field(BYTE *, char *, char *, KV_T *, LOGGER_T *);
void add_boolean_field(BYTE *, char *);
RC convert_to_ebcdic(char *, char *, char[], int, LOGGER_T *);
KV_CTL_T *uadmin_kv_init(LOGGER_T *);
KV_CTL_T *uadmin_kv_term(KV_CTL_T *);
RC key_val_to_kv(KV_CTL_T *, BYTE *, int, CCSID, BYTE *, int, CCSID);
RC segments_to_kv(KV_CTL_T *, R_ADMIN_SDESC_T *, int, iconv_t);
RC fields_to_kv(KV_CTL_T *, R_ADMIN_FDESC_T *, int, iconv_t);
RC uadmin_json_fields(R_ADMIN_FDESC_T *, int, BYTE *, R_ADMIN_CTL_T *);

// These eventually get re-located to r_admin_util.c ...
int realloc_json_buffer(int, R_ADMIN_CTL_T *);
int json_gen(R_ADMIN_CTL_T *, FLAG, FLAG, const char *, ...);


//
// Mainline code
//
RC uadmin_kv_to_segments(R_ADMIN_UADMIN_PARMS_T *p_uadmin_parms, KV_CTL_T *pKVCtl_req, LOGGER_T *pLog)
{    
   log_debug(pLog, "Start kv to segments.");
   RC rc = SUCCESS;
   // extract userid
   KV_T *pKV = kv_get_list(pKVCtl_req);
   KV_T *useridKV = kv_get(pKVCtl_req, pKV, "userid", pKVCtl_req->lKV_list, KEY_REQUIRED);
   KVV_T *useridpKVVal = useridKV->pKVVal_head;
   char *userid = useridpKVVal->pVal;
   int l_userid = useridpKVVal->lVal;
   log_debug(pLog, "userid: %s", userid);

   BYTE *finger = (BYTE *)p_uadmin_parms + sizeof(R_ADMIN_UADMIN_PARMS_T);
   int n_segs = 0;

   // Get BASE segment fields.
   BASE_SEGMENT_T *base_segment = calloc(1, sizeof(BASE_SEGMENT_T));
   base_segment->name = kv_get(pKVCtl_req, pKV, "name", pKVCtl_req->lKV_list, KEY_REQUIRED);
   base_segment->password = kv_get(pKVCtl_req, pKV, "password", pKVCtl_req->lKV_list, KEY_OPTIONAL);
   base_segment->owner = kv_get(pKVCtl_req, pKV, "owner", pKVCtl_req->lKV_list, KEY_OPTIONAL);
   base_segment->special = kv_get(pKVCtl_req, pKV, "special", pKVCtl_req->lKV_list, KEY_OPTIONAL);
   // check for OMVS segment fields and build OMVS segment if any found.
   if (
         base_segment->name != NULL 
            || base_segment->password != NULL 
            || base_segment->owner !=NULL 
            || base_segment->special !=NULL
      ) {
      rc = uadmin_build_base_segment(finger, base_segment, pLog);
      if (rc == FAILURE) 
         return rc;
      n_segs++;
   }

   // Get OMVS segment fields.
   OMVS_SEGMENT_T *omvs_segment = calloc(1, sizeof(OMVS_SEGMENT_T));
   omvs_segment->uid = kv_get(pKVCtl_req, pKV, "uid", pKVCtl_req->lKV_list, KEY_OPTIONAL);
   omvs_segment->home = kv_get(pKVCtl_req, pKV, "home", pKVCtl_req->lKV_list, KEY_OPTIONAL);
   omvs_segment->program = kv_get(pKVCtl_req, pKV, "program", pKVCtl_req->lKV_list, KEY_OPTIONAL);
   // check for OMVS segment fields and build OMVS segment if any found.
   if (omvs_segment->uid != NULL || omvs_segment->home != NULL || omvs_segment->program != NULL) {
      rc = uadmin_build_omvs_segment(finger, omvs_segment, pLog);
      if (rc == FAILURE) 
         return rc;
      n_segs++;
   }

   // Build Request Header
   char EBC_userid[l_userid];
   rc = convert_to_ebcdic("Userid", userid, EBC_userid, l_userid, pLog);
   if (rc == FAILURE)
      return rc;
   memcpy(p_uadmin_parms->userid, EBC_userid, l_userid);
   p_uadmin_parms->l_userid = l_userid;
   p_uadmin_parms->n_segs = n_segs;
   p_uadmin_parms->off_seg_1 = sizeof(R_ADMIN_UADMIN_PARMS_T);
   uadmin_print(p_uadmin_parms, pLog);
   return SUCCESS;
}

RC uadmin_build_base_segment(BYTE *finger, BASE_SEGMENT_T *base_segment, LOGGER_T *pLog) {
   RC rc = SUCCESS;
   // Build BASE segment header
   int field_count = count_base_segment_fields(base_segment);
   build_segment_header(finger, &EBCDIC_BASE_KEY, field_count);
   // Add segment fields
   if (base_segment->name != NULL) {
      rc = add_key_value_field(finger, "name", EBCDIC_NAME_KEY, base_segment->name, pLog);
      if (rc == FAILURE)
         return rc;
   }
   if (base_segment->password != NULL) {
      rc = add_key_value_field(finger, "password", EBCDIC_PASSWORD_KEY, base_segment->password, pLog);
      if (rc == FAILURE)
         return rc;
   }
   if (base_segment->owner != NULL) {
      rc = add_key_value_field(finger, "owner", EBCDIC_OWNER_KEY, base_segment->owner, pLog);
      if (rc == FAILURE)
         return rc;
   }
   if (base_segment->special != NULL)
      add_boolean_field(finger, EBCDIC_SPECIAL_KEY);
   return SUCCESS;
}

int count_base_segment_fields(BASE_SEGMENT_T *base_segment) {
   int field_count = 0;
   if (base_segment->name != NULL)
      field_count++;
   if (base_segment->password != NULL)
      field_count++;
   if (base_segment->owner != NULL)
      field_count++;
   if (base_segment->special != NULL)
      field_count++;
   return field_count;
}

RC uadmin_build_omvs_segment(BYTE *finger, OMVS_SEGMENT_T *omvs_segment, LOGGER_T *pLog) {
   RC rc = SUCCESS;
   // Build OMVS segment header
   int field_count = count_omvs_segment_fields(omvs_segment);
   build_segment_header(finger, EBCDIC_OMVS_KEY, field_count);
   // Add segment fields
   if (omvs_segment->uid != NULL) {
      rc = add_key_value_field(finger, "uid", EBCDIC_UID_KEY, omvs_segment->uid, pLog);
      if (rc == FAILURE)
         return rc;
   }
   if (omvs_segment->home != NULL) {
      rc = add_key_value_field(finger, "home", EBCDIC_HOME_KEY, omvs_segment->home, pLog);
      if (rc == FAILURE)
         return rc;
   }
   if (omvs_segment->program != NULL) {
      rc = add_key_value_field(finger, "program", EBCDIC_PROGRAM_KEY, omvs_segment->program, pLog);
      if (rc == FAILURE)
         return rc;
   }
   return SUCCESS;
}

int count_omvs_segment_fields(OMVS_SEGMENT_T *omvs_segment) {
   int field_count = 0;
   if (omvs_segment->uid != NULL)
      field_count++;
   if (omvs_segment->home != NULL)
      field_count++;
   if (omvs_segment->program != NULL)
      field_count++;
   return field_count;
}

void build_segment_header(BYTE *finger, char *ebcdic_key, int field_count) {
   // Set segment key
   int ebcdic_key_length = sizeof(ebcdic_key);
   memcpy(finger, ebcdic_key, ebcdic_key_length);
   finger += ebcdic_key_length;
   // Set create flag
   memcpy(finger, YES_FLAG, 1);
   finger++;
   // Set field count
   memcpy(finger, &field_count, sizeof(int));
   finger += sizeof(int);
}

RC add_key_value_field(BYTE *finger, char *eye_catcher, char *ebcdic_key, KV_T *pKV, LOGGER_T *pLog) {
   RC rc = SUCCESS;
   // Set key
   int l_ebcdic_key = sizeof(ebcdic_key);
   memcpy(finger, ebcdic_key, l_ebcdic_key);
   finger += l_ebcdic_key;
   // Set create flag
   memcpy(finger, YES_FLAG, 1);
   finger++;
   // extract value and length
   KVV_T *pKVV = pKV->pKVVal_head;
   char *value = pKVV->pVal;
   int l_value = pKVV->lVal;
   // Set length
   memcpy(finger, &l_value, sizeof(int));
   finger += sizeof(int)
   // Convert value to EBCDIC.
   char ebcdic_value[l_value];
   rc = convert_to_ebcdic(eye_catcher, value, ebcdic_value, l_value, pLog);
   if (rc == FAILURE)
      return rc;
   // Set value
   memcpy(finger, ebcdic_value, l_value);
   finger += l_value;
   return rc
}

void add_boolean_field(BYTE *finger, char *ebcdic_key) {
   // Set key
   int l_ebcdic_key = sizeof(ebcdic_key);
   memcpy(finger, ebcdic_key, l_ebcdic_key);
   finger += l_ebcdic_key;
   // Set create flag
   memcpy(finger, YES_FLAG, 1);
   finger++;
   // length is zero for boolean fields
   int length = 0;
   // Set length
   memcpy(finger, &length, sizeof(int));
   finger += sizeof(int)
}

RC convert_to_ebcdic(char *eye_catcher, char *ascii_string, char ebcdic_buffer[], int l_string, LOGGER_T *pLog) {
   memset(&(ebcdic_buffer[0]), 0, l_string);
   RC rc = tc_a2e(ascii_string, &(ebcdic_buffer[0]), l_string, pLog);
   if (rc == SUCCESS) {
      log_debug(pLog, "%s folded, converted to EBCDIC.", eye_catcher);
   }
   else {
      log_error(pLog, "Unable to convert %s to EBCDIC.", eye_catcher);
      return FAILURE;
   }
   return SUCCESS;
}

KV_CTL_T *results_to_kv(UADMIN_CTL_T *pUADMINCtl, R_ADMIN_UADMIN_PARMS_T *pUADMIN_results)
   {
    KV_CTL_T *pKVCtl = uadmin_kv_init(pUADMINCtl->pLog);
    RC rc = SUCCESS;
   /*
    if (pKVCtl != NULL)
       {
        BYTE *pResults = (BYTE *)pUADMIN_results + sizeof(R_ADMIN_UADMIN_PARMS_T);
        R_ADMIN_SDESC_T *pSeg1 = (R_ADMIN_SDESC_T *)(pResults + pUADMIN_results->lProf_name);
        char ASC_key_name[9];
        int  lKey;

        log_set_name(pKVCtl->pLog, "uadmin_kv");
        log_debug(pKVCtl->pLog, "Building results key/value list ...");

        // The output buffer begins with the UADMIN control area returned from RACF,
        // and the beginning of this buffer is the base from which all offsets in
        // the return data are calculated.
        pKVCtl->pOffset_base = (BYTE *)pUADMIN_results;

        // Build the class and profile names.
        memset(ASC_key_name, 0x00, sizeof(ASC_key_name));
        strcpy(ASC_key_name, "class");
        lKey = strlen(ASC_key_name);

        if ((rc = key_val_to_kv(pKVCtl, 
                               &(ASC_key_name[0]), lKey, CCSID_ASCII,
                               &(pUADMIN_results->class_name[0]), sizeof(pUADMIN_results->class_name), 
                               CCSID_EBCDIC)) == SUCCESS)
           {
            memset(ASC_key_name, 0x00, sizeof(ASC_key_name));
            strcpy(ASC_key_name, "profile");
            lKey = strlen(ASC_key_name);

            if ((rc = key_val_to_kv(pKVCtl, 
                                    &(ASC_key_name[0]), lKey, CCSID_ASCII,
                                    pResults, pUADMIN_results->lProf_name, 
                                    CCSID_EBCDIC)) == SUCCESS)
               {

                // Traverse the segment list in the results, and build key-value
                // pairs to represent them.
                if ((rc = segments_to_kv(pKVCtl, 
                                         pSeg1, pUADMIN_results->nSegments, pKVCtl->tc_e2a_cd)) == SUCCESS)
                   {
                    // log_debug(pKVCtl->pLog, "--- Current KV list: -----------");
                    // kv_print(pKVCtl);
                    // log_debug(pKVCtl->pLog, "--------------------------------");
                   }      

               }

           }

       }

    else
       rc = FAILURE;
    if (rc != SUCCESS)
       pKVCtl = uadmin_kv_term(pKVCtl);
   */
    return pKVCtl;
   }                                   // results_to_kv




KV_CTL_T *uadmin_kv_init(LOGGER_T *pLog)
   {
    KV_CTL_T *pKVCtl = kv_init(pLog);

    // Initialize transcoders for converting character strings to/from ASCII and
    // EBCDIC as the KV list is built.
    pKVCtl->tc_a2e_cd = tc_init(CCSID_EBCDIC, CCSID_ASCII, pLog);
    pKVCtl->tc_e2a_cd = tc_init(CCSID_ASCII, CCSID_EBCDIC, pLog);

    if ((pKVCtl->tc_a2e_cd < 0) || (pKVCtl->tc_e2a_cd < 0))
       {
        kv_term(pKVCtl);
        pKVCtl = NULL;
       }

    return pKVCtl;
   }                                   // uadmin_kv_init


KV_CTL_T *uadmin_kv_term(KV_CTL_T *pKVCtl)
   {

    tc_term(pKVCtl->tc_a2e_cd, pKVCtl->pLog);
    tc_term(pKVCtl->tc_e2a_cd, pKVCtl->pLog);
    kv_term(pKVCtl);
    pKVCtl = NULL;

    return pKVCtl;
   }                                   // uadmin_kv_term
 

 // Some data returned from RACF has a value, but with no associated key/name.
 // The caller will supply a key, while the value will usually come from the results
 // returned from RACF.
 RC key_val_to_kv(KV_CTL_T *pKVCtl_res, 
                  BYTE *pKey, int lKey, CCSID key_enc, 
                  BYTE *pVal, int lVal, CCSID val_enc) 
   {
    RC rc;

    if ((rc = kv_add(pKVCtl_res, pKey, lKey, key_enc, VAL_DIM_NONE)) == SUCCESS)
       rc = kv_add_value(pKVCtl_res, pVal, lVal, val_enc, VAL_TYPE_TXT);

    return rc;
   }                                   // key_val_to_kv


RC segments_to_kv(KV_CTL_T *pKVCtl_res, R_ADMIN_SDESC_T *pSeg_1, int nSegs, iconv_t tc_e2a_cd)
   {
    R_ADMIN_SDESC_T *pSeg = pSeg_1;
    int iSeg = 1;
    RC rc = SUCCESS;

    log_debug(pKVCtl_res->pLog, "Generate KV pairs for %d segments", nSegs);
    // dump_mem(pKVCtl_res->pOffset_base, 384, CCSID_EBCDIC, pKVCtl_res->pLog);

    while((iSeg <= nSegs) && (pSeg != NULL) && (rc == SUCCESS))
       {
        // log_debug(pKVCtl_res->pLog, "pSeg[%d]:", iSeg);
        // dump_mem(pSeg, sizeof(R_ADMIN_SDESC_T), CCSID_EBCDIC, pKVCtl_res->pLog);

        if ((rc = kv_add(pKVCtl_res, 
                         pSeg->name, sizeof(pSeg->name), 
                         CCSID_EBCDIC, VAL_DIM_NEST)) == SUCCESS)
           {
            kv_nest(pKVCtl_res);

            if ((rc = fields_to_kv(pKVCtl_res, 
                                  (R_ADMIN_FDESC_T *)(pKVCtl_res->pOffset_base+pSeg->off_fdesc_1), 
                                  pSeg->nFields, tc_e2a_cd)) == SUCCESS)
               {
                iSeg++;
                pSeg++;
               }

            kv_unnest(pKVCtl_res);

            // Add a NULL KV pair to indicate that this is the end of a nesting.  This
            // is important to know when generating json from a KV list.
            rc = kv_add(pKVCtl_res, 
                        NULL, 0, 
                        CCSID_EBCDIC, VAL_DIM_NONE);
           }

        iSeg = nSegs + 1;
       }

    return rc;
   }                                   // segments_to_kv


RC fields_to_kv(KV_CTL_T *pKVCtl_res, R_ADMIN_FDESC_T *pFld_1, int nFlds, iconv_t tc_e2a_cd)
   {
    R_ADMIN_FDESC_T *pFld = pFld_1;
    char ASC_true[5];
    char ASC_false[6];
    int  iFld = 1;
    RC   rc = SUCCESS;

    log_debug(pKVCtl_res->pLog, "Generate KV pairs for %d fields ...", nFlds);
    strcpy(ASC_true, "true");
    strcpy(ASC_false, "false");

    while((iFld <= nFlds) && (pFld != NULL) && (rc == SUCCESS))
       {
        BYTE *pValue;
        int   lValue;
        CCSID eValue;

        // If this is a boolean field, use the string mnemonics for the field
        // valye.
        if (pFld->type & t_boolean_field)
           {

            if (pFld->flags & f_boolean_field)
               {
                pValue = &(ASC_true[0]);
                lValue = sizeof(ASC_true);
               }

            else
               {
                pValue = &(ASC_false[0]);
                lValue = sizeof(ASC_false);
               }

            eValue = CCSID_ASCII;
           }

        // All non-boolean values are located at an offset from the global
        // base pointer.
        else
           {
            pValue = pKVCtl_res->pOffset_base + pFld->off_rpt.off_fld_data;
            lValue = pFld->len_rpt.l_fld_data;
            eValue = CCSID_EBCDIC;
           }
 

        if ((rc = key_val_to_kv(pKVCtl_res, 
                               &(pFld->name[0]), sizeof(pFld->name), CCSID_EBCDIC,
                               pValue, lValue, eValue)) == SUCCESS)
           {
            iFld++;
            pFld++;
           }

       }

    return rc;
   }                                   // fields_to_kv


RC uadmin_json_fields(R_ADMIN_FDESC_T *p_fdesc, int nFields, 
                    BYTE *pParms, R_ADMIN_CTL_T *pRACtl)
   {
    int i_fld = 1;
    char fld_name[9];                  // var for null-terminating strings
    R_ADMIN_FDESC_T *p_fld = p_fdesc;
    RC rc = SUCCESS;

    if (json_gen(pRACtl, FALSE, TRUE, "          \"fields\": [") < 0)
       return FAILURE;

    while(i_fld <= nFields)
      {
       FLAG f_comma = TRUE;

       memset(fld_name, 0, sizeof(fld_name));
       strncpy(fld_name, p_fld->name, sizeof(p_fld->name));

       if (json_gen(pRACtl, FALSE, TRUE, "               {") < 0)
          return FAILURE;
       if (json_gen(pRACtl, TRUE, TRUE, "                \"name\": \"%s\"", fld_name) < 0)
          return FAILURE;
       if (json_gen(pRACtl, TRUE, TRUE, "                \"type\": \"%04x\"", p_fld->type) < 0)
          return FAILURE;
       if (json_gen(pRACtl, TRUE, TRUE, "                \"flags\": \"%08x\"", p_fld->flags) < 0)
          return FAILURE;

       if (!(p_fld->type & t_repeat_field_hdr))
         {                             // single value field
          char content[1025];          // null-terminated string
          int l_content = sizeof(content);

          // Null-terminate, and clip the size of the content if necessary.
          memset(content, 0, sizeof(content));
          if (p_fld->len_rpt.l_fld_data < sizeof(content))
            l_content = p_fld->len_rpt.l_fld_data;
          strncpy(content, ((char *)pParms)+p_fld->off_rpt.off_fld_data, l_content);
          if (json_gen(pRACtl, FALSE, TRUE, "                \"content\": \"%s\"", content) < 0)
             return FAILURE;
         }                             // single value field

       else
         {                             // repeating field

          if (pRACtl->fDebug)
             {
              printf("---   num repeat grps:  %d\n",p_fld->len_rpt.n_repeat_grps);
              printf("---   num repeat elems: %d\n",p_fld->off_rpt.n_repeat_elems);
             }

         }                             // repeating field

       // No comma after the last field.
       if (i_fld >= nFields)
          f_comma = FALSE;
       if (json_gen(pRACtl, f_comma, TRUE, "               }") < 0)
          return FAILURE;

       i_fld++;
       p_fld++;
      }

    if (json_gen(pRACtl, FALSE, TRUE, "             ]") < 0)
       return FAILURE;

    return rc;
   }                                   // uadmin_json_fields


// Estimate the size of the json buffer needed from the control
// blocks returned by R_admin, and allocate the buffer.
int realloc_json_buffer(int l_output_buf_req, R_ADMIN_CTL_T *pRACtl)
   {                                   // realloc_json_buffer
    int   l_jbuf = pRACtl->l_jbuf;
    char *p_jbuf;

    printf("--- uadmin_json [1]: l_output_buf_req: %d, l_jbuf: %d\n", l_output_buf_req, l_jbuf);

    // If the buffer hasn't yet been allocated, do so now based
    // on the length of the address/offset/length R_admin output
    // buffer.  Otherwise, grow based on the length of the existing 
    // json buffer.
    if (!pRACtl->p_jbuf)
       l_jbuf = (int)((float)l_output_buf_req * JSON_BUFFER_RATIO);

    // Keep growing the buffer until it's big enough to satisfy
    // the request.
    else
        {
         while(l_output_buf_req > l_jbuf)
            l_jbuf = (int)((float)l_jbuf * JSON_BUFFER_RATIO);
        }

    l_jbuf = l_jbuf + 16 - (l_jbuf % 16); // make things tidy
    p_jbuf = (char *)malloc(l_jbuf);

    if (p_jbuf)
       {
        if (pRACtl->fDebug)
           printf("--- JSON buffer allocated: %d bytes\n", l_jbuf);

        // Re-allocating, copy current contents to new buffer, and 
        // adjust the JSON generation state.
        if (pRACtl->p_jbuf)
           {
            int l_jbuf_used = pRACtl->p_jbuf_finger - pRACtl->p_jbuf;

            if (pRACtl->fDebug)
               printf("--- p_jbuf: %08x, finger: %08x, l_jbuf_used: %d\n", 
                      pRACtl->p_jbuf, pRACtl->p_jbuf_finger, l_jbuf_used);

            memcpy(p_jbuf, pRACtl->p_jbuf, l_jbuf_used);
            pRACtl->p_jbuf_finger =  p_jbuf + l_jbuf_used;
            pRACtl->l_jbuf = l_jbuf;
            pRACtl->l_jbuf_free = l_jbuf - l_jbuf_used;

            if (pRACtl->fDebug)
               printf("--- p_jbuf: %08x, finger: %08x, l_jbuf: %d, l_jbuf_free: %d\n", 
                      pRACtl->p_jbuf, pRACtl->p_jbuf_finger, pRACtl->l_jbuf, pRACtl->l_jbuf_free);
           }

        else
           {
            pRACtl->p_jbuf_finger = p_jbuf;
            pRACtl->l_jbuf_free = l_jbuf;
           }

        pRACtl->p_jbuf = p_jbuf;
        pRACtl->l_jbuf = l_jbuf;

        if (pRACtl->fDebug)
           printf("--- p_jbuf: %08x, p_jbuf_finger: %08x, l_jbuf: %d, l_jbuf_free: %d\n", 
                  pRACtl->p_jbuf, pRACtl->p_jbuf_finger, pRACtl->l_jbuf, pRACtl->l_jbuf_free);
       }

    else
       {
        fprintf(stderr, "Error, allocate JSON buffer failed\n");
        l_jbuf = 0;
       }

    return l_jbuf;
   }                                   // realloc_json_buffer


// Write a line to the json output buffer.
int json_gen(R_ADMIN_CTL_T *pRACtl, FLAG f_comma, FLAG f_nl, const char *fmt, ...)
   {
    int l_written;
    char out_jbuf[1025];
    va_list args;

    va_start(args, fmt);
    l_written = vsprintf(out_jbuf, fmt, args);
    va_end(args);

    // Append this formatted string to the JSON buffer.
    if (l_written > 0)
       {
        FLAG f_buf_has_space = TRUE;

        // Make sure we won't overflow the JSON buffer with this output.
        if (l_written >= pRACtl->l_jbuf_free)
           {
            if (realloc_json_buffer((l_written + (pRACtl->l_jbuf)), pRACtl) <= 0)
               f_buf_has_space = FALSE;
           }
       
        if (f_buf_has_space)
           { 
            if (pRACtl->fDebug)
               printf("--- Write %d bytes, %d bytes free\n", l_written, pRACtl->l_jbuf_free);
            memcpy(pRACtl->p_jbuf_finger, out_jbuf, l_written);
            pRACtl->p_jbuf_finger += l_written;
            pRACtl->l_jbuf_free -= l_written;

            // Append a comma if requested.
            if (f_comma)
               {
                memcpy(pRACtl->p_jbuf_finger, ",", 1);
                ++(pRACtl->p_jbuf_finger);
                --(pRACtl->l_jbuf_free);
               }

            // Append a newline if requested.
            if (f_nl)
               {
                memcpy(pRACtl->p_jbuf_finger, "\n", 1);
                ++(pRACtl->p_jbuf_finger);
                --(pRACtl->l_jbuf_free);
               }

           }

       }

    else
       fprintf(stderr, "Error, write to JSON buffer failed\n");

    return l_written;
   }
