/*
 *  This Source Code Form is subject to the terms of the Mozilla Public
 *  License, v. 2.0. If a copy of the MPL was not distributed with this
 *  file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 */

#include <npnt_internal.h>
#include <npnt.h>
#include <mxml/mxml.h>

/**
 * @brief   Sets Current Permission Artifact.
 * @details This method consumes peremission artefact in raw format
 *          and sets up npnt structure.
 *
 * @param[in] npnt_handle       npnt handle
 * @param[in] permart           permission json artefact in base64 format as received
 *                              from server
 * @param[in] permart_length    size of permission json artefact in base64 format as received
 *                              from server
 * @param[in] signature         signature of permart in base64 format
 * @param[in] signature_length  length of the signature of permart in base64 format 
 * 
 * @return           Error id if faillure, 0 if no breach
 * @retval NPNT_INV_ART   Invalid Artefact
 *         NPNT_INV_AUTH  signed by unauthorised entity
 *         NPNT_INV_STATE artefact can't setup in current aircraft state
 *         NPNT_ALREADY_SET artefact already set, free previous artefact first
 * @iclass control_iface
 */
int8_t npnt_set_permart(npnt_s *handle, uint8_t *permart, uint16_t permart_length, uint8_t base64_encoded)
{
    if (!handle) {
        return NPNT_UNALLOC_HANDLE;
    }
    int16_t ret = 0;
    //Extract XML from base64 encoded permart
    if (handle->raw_permart) {
    	printf("PA already set\n");
        return NPNT_ALREADY_SET;
    }

    if (base64_encoded) {
        handle->raw_permart = (char*)base64_decode(permart, permart_length, &handle->raw_permart_len);
        if (!handle->raw_permart) {
        	printf("PA Parse failed\n");
            return NPNT_PARSE_FAILED;
        }
    } else {
        handle->raw_permart = (char*)malloc(permart_length);
        if (!handle->raw_permart) {
        	printf("PA parse failed\n");
            return NPNT_PARSE_FAILED;
        }
        memcpy(handle->raw_permart, permart, permart_length);
    }

    //parse XML permart
    handle->parsed_permart = mxmlLoadString(NULL, handle->raw_permart, MXML_OPAQUE_CALLBACK);
    if (!handle->parsed_permart) {
    	printf("PA parse failed_\n");
        return NPNT_PARSE_FAILED;
    }

    //Verify Artifact against Sender's Public Key
    ret = npnt_verify_permart(handle);
    if (ret < 0) {
    	printf("verify failed: %d\n", ret);
        return ret;
    }

    //Collect Fence points from verified artefact
    ret = npnt_alloc_and_get_fence_points(handle, handle->fence.vertlat, handle->fence.vertlon);
    if (ret <= 0) {
        handle->fence.nverts = 0;
        printf("bad fence PA\n");
        return NPNT_BAD_FENCE;
    }
    handle->fence.nverts = ret;
    ret = 0;

    // Max alt has been removed for new PA

    //Set Flight Params from artefact
    ret = npnt_populate_flight_params(handle);
    if (ret < 0) {
    	printf("bag PA params\n");
        handle->fence.nverts = 0;
        return NPNT_INV_FPARAMS;
    }
    ret = 0;
    return ret;
}

//Verify the data contained in parsed XML
int8_t npnt_verify_permart(npnt_s *handle)
{
    char* raw_perm_without_sign;
    char* signed_info;
    const uint8_t* rcvd_digest_value;
    // char *test_str;
    int16_t permission_length, signedinfo_length;
    char digest_value[32];
    const uint8_t* signature = NULL;
    uint8_t* raw_signature = NULL;
    uint16_t signature_len, raw_signature_len;
    uint8_t* base64_digest_value = NULL;
    uint16_t base64_digest_value_len;
    uint16_t curr_ptr = 0, curr_length;
    char last_empty_element[20];
    int8_t ret = 0;
    
    reset_sha256();
    
    //Digest Signed Info
    update_sha256("<SignedInfo xmlns=\"http://www.w3.org/2000/09/xmldsig#\">",
                strlen("<SignedInfo xmlns=\"http://www.w3.org/2000/09/xmldsig#\">"));
    signed_info = strstr(handle->raw_permart, "<SignedInfo>") + strlen("<SignedInfo>");
    if (signed_info == NULL) {
    	printf("hi1\n");
        ret = NPNT_INV_ART;
        goto fail;
    }
    signedinfo_length = strstr(handle->raw_permart, "<SignatureValue") - signed_info;
    if (signedinfo_length < 0) {
    	printf("hi2\n");
        ret = NPNT_INV_ART;
        goto fail;
    }
    while (curr_ptr < signedinfo_length) {
        curr_length = 1;
        if (signed_info[curr_ptr] == '<') {
            while((curr_ptr + curr_length) < signedinfo_length) {
                if (signed_info[curr_ptr + curr_length] == ' ') {
                    last_empty_element[curr_length - 1] = '\0';
                    break;
                } else if (signed_info[curr_ptr + curr_length] == '>') {
                    last_empty_element[0] = '\0';
                    break;
                }
                last_empty_element[curr_length - 1] = signed_info[curr_ptr + curr_length];
                curr_length++;
            }
        }

        if (strlen(last_empty_element) != 0) {
            if (signed_info[curr_ptr] == '/') {
                if (signed_info[curr_ptr + 1] == '>') {
                    update_sha256("></", 3);
                    update_sha256(last_empty_element, strlen(last_empty_element));
                    last_empty_element[0] = '\0';
                    curr_ptr += curr_length;
                    continue;
                }
            }
        }

        update_sha256(&signed_info[curr_ptr], curr_length);
        curr_ptr += curr_length;
    }
    final_sha256(digest_value);

    //fetch SignatureValue from xml
    signature = (const uint8_t*)mxmlGetOpaque(mxmlFindElement(handle->parsed_permart, handle->parsed_permart, "SignatureValue", NULL, NULL, MXML_DESCEND));
    if (signature == NULL) {
    	printf("hi3\n");
    	ret = NPNT_INV_SIGN;
        goto fail;
    }
    signature_len = strlen(signature);
    raw_signature = base64_decode(signature, signature_len, &raw_signature_len);
    //Check authenticity of the artifact
    if (npnt_check_authenticity(handle, digest_value, 32, raw_signature, raw_signature_len) != 1) {
    	printf("hi4\n");
        ret = NPNT_INV_AUTH;
        goto fail;
    }

    //Digest Canonicalised Permission Artifact
    raw_perm_without_sign = strstr(handle->raw_permart, "<UAPermission");
    if (raw_perm_without_sign == NULL) {
    	printf("hi5\n");
        ret = NPNT_INV_ART;
        goto fail;
    }
    permission_length = strstr(handle->raw_permart, "<Signature") - raw_perm_without_sign;
    if (permission_length < 0) {
    	printf("hi6\n");
        ret = NPNT_INV_ART;
        goto fail;
    }
    // test_str = (char*)malloc(permission_length + 1);
    // memcpy(test_str, raw_perm_without_sign, permission_length);
    // test_str[permission_length] = '\0';
    // printf("\n RAW PERMISSION: \n%s", test_str);

    reset_sha256();
    curr_ptr = 0;
    curr_length = 0;
    //Canonicalise Permission Artefact by converting Empty elements to start-end tag pairs
    while (curr_ptr < permission_length) {
        curr_length = 1;
        if (raw_perm_without_sign[curr_ptr] == '<') {
            while((curr_ptr + curr_length) < permission_length) {
                if (raw_perm_without_sign[curr_ptr + curr_length] == ' ') {
                    last_empty_element[curr_length - 1] = '\0';
                    break;
                } else if (raw_perm_without_sign[curr_ptr + curr_length] == '>') {
                    last_empty_element[0] = '\0';
                    break;
                }
                last_empty_element[curr_length - 1] = raw_perm_without_sign[curr_ptr + curr_length];
                curr_length++;
            }
        }

        if (strlen(last_empty_element) != 0) {
            if (raw_perm_without_sign[curr_ptr] == '/') {
                if (raw_perm_without_sign[curr_ptr + 1] == '>') {
                    update_sha256("></", 3);
                    update_sha256(last_empty_element, strlen(last_empty_element));
                    last_empty_element[0] = '\0';
                    curr_ptr += curr_length;
                    continue;
                }
            }
        }

        update_sha256(&raw_perm_without_sign[curr_ptr], curr_length);
        curr_ptr += curr_length;
    }

    //Skip Signature for Digestion
    //Skip Signature for Digestion
    raw_perm_without_sign = strstr(handle->raw_permart, "</Signature>") + strlen("</Signature>");
    uint16_t valid_count = 0;
    for(uint16_t i=0; i< strlen(raw_perm_without_sign); i++){
    	if(raw_perm_without_sign[i] == '>'){
    		valid_count = i+1;
    		break;
    	}
    }
    update_sha256(raw_perm_without_sign, valid_count);
    final_sha256(digest_value);
    base64_digest_value = base64_encode(digest_value, 32, &base64_digest_value_len);
    // printf("\nDigest Value: \n%s\n", base64_digest_value);
    // printf("\nDigest Value: \n%s\n", mxmlGetOpaque(mxmlFindElement(handle->parsed_permart, handle->parsed_permart, "DigestValue", NULL, NULL, MXML_DESCEND)));
    
    //Check Digestion
    rcvd_digest_value = (const uint8_t*)mxmlGetOpaque(mxmlFindElement(handle->parsed_permart, handle->parsed_permart, "DigestValue", NULL, NULL, MXML_DESCEND));
    for (uint16_t i = 0; i < base64_digest_value_len - 1; i++) {
        if (base64_digest_value[i] != rcvd_digest_value[i]) {
            ret = NPNT_INV_DGST;
        	printf("hi7\n");
            goto fail;
        }
    }

    //base64_digest_value no longer needed
    free(base64_digest_value);
    base64_digest_value = NULL;
fail:
    if (base64_digest_value) {
        free(base64_digest_value);
    }
    return ret;
}

int8_t npnt_alloc_and_get_fence_points(npnt_s* handle, float* vertlat, float* vertlon)
{
    //Calculate number of vertices
    mxml_node_t *first_coordinate, *current_coordinate;
    uint16_t nverts = 0;
    const char* lat_str;
    const char* lon_str;
    first_coordinate = mxmlGetFirstChild(mxmlFindElement(handle->parsed_permart, handle->parsed_permart, "Coordinates", NULL, NULL, MXML_DESCEND));
    current_coordinate = first_coordinate;
    while (current_coordinate) {
        if (mxmlGetElement(current_coordinate) == NULL) {
            current_coordinate = mxmlGetNextSibling(current_coordinate);
            continue;
        }
        if (strcmp(mxmlGetElement(current_coordinate), "Coordinate") != 0) {
            current_coordinate = mxmlGetNextSibling(current_coordinate);
            continue;
        }
        current_coordinate = mxmlGetNextSibling(current_coordinate);
        nverts++;
    }

    //Allocate vertices
    vertlat = (float*)malloc(nverts*sizeof(float));
    vertlon = (float*)malloc(nverts*sizeof(float));

    if (!vertlat || !vertlon) {
        return -1;
    }
    //read coordinates
    nverts = 0;
    current_coordinate = first_coordinate;
    while(current_coordinate) {
        if (mxmlGetElement(current_coordinate) == NULL) {
            current_coordinate = mxmlGetNextSibling(current_coordinate);
            continue;
        }
        if (strcmp(mxmlGetElement(current_coordinate), "Coordinate") != 0) {
            current_coordinate = mxmlGetNextSibling(current_coordinate);
            continue;
        }
        lat_str = mxmlElementGetAttr(current_coordinate, "latitude");
        if (lat_str) {
            vertlat[nverts] = atof(lat_str);
        } else {
            goto fail;
        }
        lon_str = mxmlElementGetAttr(current_coordinate, "longitude");
        if (lon_str) {
            vertlon[nverts] = atof(lon_str);
        } else {
            goto fail;
        }
        current_coordinate = mxmlGetNextSibling(current_coordinate);
        nverts++;
    }

    handle->fence.vertlat = malloc(sizeof(vertlat));
    handle->fence.vertlon = malloc(sizeof(vertlon));
    handle->fence.vertlat = vertlat;
    handle->fence.vertlon = vertlon;

    return nverts;
fail:
    free(vertlat);
    free(vertlon);
    return -1;
}

int8_t npnt_get_max_altitude(npnt_s* handle, float* altitude)
{
    mxml_node_t* flightparams;
    const char* alt_str;
    if (!altitude) {
        return -1;
    }
    flightparams = mxmlFindElement(handle->parsed_permart, handle->parsed_permart, "FlightParameters", NULL, NULL, MXML_DESCEND);
    if (flightparams == NULL) {
        return -1;
    }
    alt_str = mxmlElementGetAttr(flightparams, "maxAltitude");
    if (alt_str) {
        *altitude = atof(alt_str);
        // printf("Altitude: %f\n", *altitude);
    } else {
        return -1;
    }
    return 0;
}

int8_t npnt_ist_date_time_to_unix_time(const char* dt_string, struct tm* date_time)
{
    char data[5] = {};
    if (strlen(dt_string) != 32) {
        return -1;
    }
    if (!date_time) {
        return -1;
    }
    memset(date_time, 0, sizeof(struct tm));

    //read year
    memcpy(data, dt_string, 4);
    data[4] = '\0';
    date_time->tm_year = atoi(data);
    //read month
    memcpy(data, &dt_string[5], 2);
    data[2] = '\0';
    date_time->tm_mon = atoi(data);
    //read day
    memcpy(data, &dt_string[8], 2);
    data[2] = '\0';
    date_time->tm_mday = atoi(data);
    //read hour
    memcpy(data, &dt_string[11], 2);
    data[2] = '\0';
    date_time->tm_hour = atoi(data);
    //read minute
    memcpy(data, &dt_string[14], 2);
    data[2] = '\0';
    date_time->tm_min = atoi(data);
    //read second
    memcpy(data, &dt_string[17], 2);
    data[2] = '\0';
    date_time->tm_sec = atoi(data);

    return 0;
    // time_t tim = mktime(date_time);
    // printf("\nUnixTime: %s\n", ctime(&tim));
}

char* npnt_get_attr(mxml_node_t *node, const char* attr)
{
    const char* tmp = NULL;
    char* ret = NULL;
    tmp = mxmlElementGetAttr(node, attr);
    if (!tmp) {
        return NULL;
    }
    ret = (char*)malloc(strlen(tmp) + 1);
    if (!ret) {
        return NULL;
    }
    strcpy(ret, tmp);
    return ret;
}

int8_t npnt_populate_flight_params(npnt_s* handle)
{
    mxml_node_t *ua_detail, *flight_params;
    ua_detail = mxmlFindElement(handle->parsed_permart, handle->parsed_permart, "UADetails", NULL, NULL, MXML_DESCEND);
    if (!ua_detail) {
        return NPNT_INV_FPARAMS;
    }
    flight_params = mxmlFindElement(handle->parsed_permart, handle->parsed_permart, "FlightParameters", NULL, NULL, MXML_DESCEND);
    if (!flight_params) {
        return NPNT_INV_FPARAMS;
    }

    handle->params.uinNo = npnt_get_attr(ua_detail, "uinNo");
    if (!handle->params.uinNo) {
        return NPNT_INV_FPARAMS;
    }

    if (npnt_ist_date_time_to_unix_time(mxmlElementGetAttr(flight_params, "flightEndTime"), &handle->params.flightEndTime) < 0) {
        return NPNT_INV_FPARAMS;
    }
    if (npnt_ist_date_time_to_unix_time(mxmlElementGetAttr(flight_params, "flightStartTime"), &handle->params.flightStartTime) < 0) {
        return NPNT_INV_FPARAMS;
    }
    return 0;
}

int8_t sign_log_data(npnt_s *handle, uint8_t *log, uint16_t log_size, char* digest_value){
	//char digest_value[32];
	//uint8_t* signature;
	char* unsigned_info = (char *)log;

	uint8_t* signature = NULL;
	uint16_t signature_len = 256;

	reset_sha256();
	update_sha256(log, log_size);
	final_sha256(digest_value);

	//__sign_hash(digest_value, 32, signature);
	free(signature);
	return 0;

}

