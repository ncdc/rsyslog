/* nsd_gtls.c
 *
 * An implementation of the nsd interface for GnuTLS.
 * 
 * Copyright (C) 2007, 2008 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * The rsyslog runtime library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The rsyslog runtime library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the rsyslog runtime library.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 * A copy of the LGPL can be found in the file "COPYING.LESSER" in this distribution.
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gcrypt.h>
#include <errno.h>

#include "rsyslog.h"
#include "syslogd-types.h"
#include "module-template.h"
#include "cfsysline.h"
#include "obj.h"
#include "stringbuf.h"
#include "errmsg.h"
#include "net.h"
#include "nsd_ptcp.h"
#include "nsdsel_gtls.h"
#include "nsd_gtls.h"

/* things to move to some better place/functionality - TODO */
#define DH_BITS 1024
#define CRLFILE "crl.pem"


GCRY_THREAD_OPTION_PTHREAD_IMPL;
MODULE_TYPE_LIB

/* static data */
DEFobjStaticHelpers
DEFobjCurrIf(errmsg)
DEFobjCurrIf(glbl)
DEFobjCurrIf(nsd_ptcp)

static int bGlblSrvrInitDone = 0;	/**< 0 - server global init not yet done, 1 - already done */

/* a macro to check GnuTLS calls against unexpected errors */
#define CHKgnutls(x) \
	if((gnuRet = (x)) != 0) { \
		uchar *pErr = gtlsStrerror(gnuRet); \
		dbgprintf("unexpected GnuTLS error %d in %s:%d: %s\n", gnuRet, __FILE__, __LINE__, pErr); \
		free(pErr); \
		ABORT_FINALIZE(RS_RET_GNUTLS_ERR); \
	}


/* ------------------------------ GnuTLS specifics ------------------------------ */
static gnutls_certificate_credentials xcred;
static gnutls_dh_params dh_params;

/* Convert a fingerprint to printable data. The  conversion is carried out
 * according IETF I-D syslog-transport-tls-12. The fingerprint string is
 * returned in a new cstr object. It is the caller's responsibility to
 * destruct that object.
 * rgerhards, 2008-05-08
 */
static rsRetVal
GenFingerprintStr(uchar *pFingerprint, size_t sizeFingerprint, cstr_t **ppStr)
{
	cstr_t *pStr = NULL;
	uchar buf[4];
	size_t i;
	int bAddColon = 0; /* do we need to add a colon to the fingerprint string? */
	DEFiRet;

	CHKiRet(rsCStrConstruct(&pStr));
	for(i = 0 ; i < sizeFingerprint ; ++i) {
		if(bAddColon) {
			CHKiRet(rsCStrAppendChar(pStr, ':'));
		} else {
			bAddColon = 1; /* all but the first need a colon added */
		}
		snprintf((char*)buf, sizeof(buf), "%2.2X", pFingerprint[i]);
		CHKiRet(rsCStrAppendStrWithLen(pStr, buf, 2));
	}
	CHKiRet(rsCStrFinish(pStr));

	*ppStr = pStr;

finalize_it:
	if(iRet != RS_RET_OK) {
		if(pStr != NULL)
			rsCStrDestruct(&pStr);
	}
	RETiRet;
}


/* a thread-safe variant of gnutls_strerror - TODO: implement it!
 * The caller must free the returned string.
 * rgerhards, 2008-04-30
 */
uchar *gtlsStrerror(int error)
{
	uchar *pErr;

	// TODO: guard by mutex!
	pErr = (uchar*) strdup(gnutls_strerror(error));

	return pErr;
}


/* add our own certificate to the certificate set, so that the peer
 * can identify us. Please note that we try to use mutual authentication,
 * so we always add a cert, even if we are in the client role (later,
 * this may be controlled by a config setting).
 * rgerhards, 2008-05-15
 */
static rsRetVal
gtlsAddOurCert(void)
{
	int gnuRet;
	uchar *keyFile;
	uchar *certFile;
	DEFiRet;

	certFile = glbl.GetDfltNetstrmDrvrCertFile();
	keyFile = glbl.GetDfltNetstrmDrvrKeyFile();
	dbgprintf("GTLS certificate file: '%s'\n", certFile);
	dbgprintf("GTLS key file: '%s'\n", keyFile);
	CHKgnutls(gnutls_certificate_set_x509_key_file(xcred, (char*)certFile, (char*)keyFile, GNUTLS_X509_FMT_PEM));

finalize_it:
	RETiRet;
}


/* globally initialize GnuTLS */
static rsRetVal
gtlsGlblInit(void)
{
	int gnuRet;
	uchar *cafile;
	DEFiRet;

	/* gcry_control must be called first, so that the thread system is correctly set up */
	gcry_control (GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
	CHKgnutls(gnutls_global_init());
	
	/* X509 stuff */
	CHKgnutls(gnutls_certificate_allocate_credentials(&xcred));

	/* sets the trusted cas file */
	cafile = glbl.GetDfltNetstrmDrvrCAF();
	dbgprintf("GTLS CA file: '%s'\n", cafile);
	gnuRet = gnutls_certificate_set_x509_trust_file(xcred, (char*)cafile, GNUTLS_X509_FMT_PEM);
	if(gnuRet < 0) {
		/* TODO; a more generic error-tracking function (this one based on CHKgnutls()) */
		uchar *pErr = gtlsStrerror(gnuRet);
		dbgprintf("unexpected GnuTLS error %d in %s:%d: %s\n", gnuRet, __FILE__, __LINE__, pErr);
		free(pErr);
		ABORT_FINALIZE(RS_RET_GNUTLS_ERR);
	}

finalize_it:
	RETiRet;
}

static rsRetVal
gtlsInitSession(nsd_gtls_t *pThis)
{
	DEFiRet;
	int gnuRet;
	gnutls_session session;

	gnutls_init(&session, GNUTLS_SERVER);
	pThis->bHaveSess = 1;
	pThis->bIsInitiator = 0;

	/* avoid calling all the priority functions, since the defaults are adequate. */
	CHKgnutls(gnutls_set_default_priority(session));
	CHKgnutls(gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, xcred));

	/* request client certificate if any.  */
	gnutls_certificate_server_set_request( session, GNUTLS_CERT_REQUEST);
	gnutls_dh_set_prime_bits(session, DH_BITS);

	pThis->sess = session;

finalize_it:
	RETiRet;
}


static rsRetVal
generate_dh_params(void)
{
	int gnuRet;
	DEFiRet;
	/* Generate Diffie Hellman parameters - for use with DHE
	 * kx algorithms. These should be discarded and regenerated
	 * once a day, once a week or once a month. Depending on the
	 * security requirements.
	 */
	CHKgnutls(gnutls_dh_params_init( &dh_params));
	CHKgnutls(gnutls_dh_params_generate2( dh_params, DH_BITS));
finalize_it:
	RETiRet;
}


/* set up all global things that are needed for server operations
 * rgerhards, 2008-04-30
 */
static rsRetVal
gtlsGlblInitLstn(void)
{
	DEFiRet;

	if(bGlblSrvrInitDone == 0) {
		/* we do not use CRLs right now, and I doubt we'll ever do. This functionality is
		 * considered legacy. -- rgerhards, 2008-05-05
		 */
		/*CHKgnutls(gnutls_certificate_set_x509_crl_file(xcred, CRLFILE, GNUTLS_X509_FMT_PEM));*/
	//CHKiRet(gtlsAddOurCert());
		CHKiRet(generate_dh_params());
		gnutls_certificate_set_dh_params(xcred, dh_params); /* this is void */
		bGlblSrvrInitDone = 1; /* we are all set now */
	}

finalize_it:
	RETiRet;
}


/* check the fingerprint of the remote peer's certificate.
 * rgerhards, 2008-05-08
 */
rsRetVal
gtlsChkFingerprint(nsd_gtls_t *pThis)
{
	cstr_t *pstrFingerprint = NULL;
	uchar fingerprint[20];
	size_t size;
	const gnutls_datum *cert_list;
	unsigned int list_size = 0;
	gnutls_x509_crt cert;
	int bMustDeinitCert = 0;
	int gnuRet;
	int bFoundPositiveMatch;
	permittedPeers_t *pPeer;
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, nsd_gtls);

	/* first check if we need to do fingerprint authentication - if not, we
	 * are already set ;) -- rgerhards, 2008-05-21
	 */
	if(pThis->authMode != GTLS_AUTH_CERTFINGERPRINT)
		FINALIZE;
	
	/* This function only works for X.509 certificates.  */
	if(gnutls_certificate_type_get(pThis->sess) != GNUTLS_CRT_X509)
		return RS_RET_TLS_CERT_ERR;

	cert_list = gnutls_certificate_get_peers(pThis->sess, &list_size);

	if(list_size < 1)
		ABORT_FINALIZE(RS_RET_TLS_NO_CERT);

	/* If we reach this point, we have at least one valid certificate. 
	 * We always use only the first certificate. As of GnuTLS documentation, the
	 * first certificate always contains the remote peer's own certificate. All other
	 * certificates are issuer's certificates (up the chain). However, we do not match
	 * against some issuer fingerprint but only ourselfs. -- rgerhards, 2008-05-08
	 */
	CHKgnutls(gnutls_x509_crt_init(&cert));
	bMustDeinitCert = 1; /* indicate cert is initialized and must be freed on exit */
	CHKgnutls(gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER));

	/* obtain the SHA1 fingerprint */
	size = sizeof(fingerprint);
	CHKgnutls(gnutls_x509_crt_get_fingerprint(cert, GNUTLS_DIG_SHA1, fingerprint, &size));
	CHKiRet(GenFingerprintStr(fingerprint, size, &pstrFingerprint));
	dbgprintf("peer's certificate SHA1 fingerprint: %s\n", rsCStrGetSzStr(pstrFingerprint));

	/* now search through the permitted peers to see if we can find a permitted one */
	bFoundPositiveMatch = 0;
	pPeer = pThis->pPermPeers;
	while(pPeer != NULL && !bFoundPositiveMatch) {
		if(!rsCStrSzStrCmp(pstrFingerprint, pPeer->pszID, strlen((char*) pPeer->pszID))) {
			bFoundPositiveMatch = 1;
		} else {
			pPeer = pPeer->pNext;
		}
	}

	if(!bFoundPositiveMatch) {
		dbgprintf("invalid peer fingerprint, not permitted to talk to it\n");
		if(pThis->bReportAuthErr == 1) {
			errno = 0;
			errmsg.LogError(NO_ERRCODE, "error: peer fingerprint '%s' unknown - we are "
					"not permitted to talk to it", rsCStrGetSzStr(pstrFingerprint));
			pThis->bReportAuthErr = 0;
		}
		ABORT_FINALIZE(RS_RET_INVALID_FINGERPRINT);
	}

finalize_it:
dbgprintf("exit fingerprint check, iRet %d\n", iRet);
	if(pstrFingerprint != NULL)
		rsCStrDestruct(&pstrFingerprint);
	if(bMustDeinitCert)
		gnutls_x509_crt_deinit(cert);

	RETiRet;
}


/* globally de-initialize GnuTLS */
static rsRetVal
gtlsGlblExit(void)
{
	DEFiRet;
	/* X509 stuff */
	gnutls_certificate_free_credentials(xcred);
	gnutls_global_deinit(); /* we are done... */
	RETiRet;
}


/* end a GnuTLS session
 * The function checks if we have a session and ends it only if so. So it can
 * always be called, even if there currently is no session.
 */
static rsRetVal
gtlsEndSess(nsd_gtls_t *pThis)
{
	int gnuRet;
	DEFiRet;

	if(pThis->bHaveSess) {
		if(pThis->bIsInitiator) {
			gnuRet = gnutls_bye(pThis->sess, GNUTLS_SHUT_RDWR);
			while(gnuRet == GNUTLS_E_INTERRUPTED || gnuRet == GNUTLS_E_AGAIN) {
				gnuRet = gnutls_bye(pThis->sess, GNUTLS_SHUT_RDWR);
			}
		}
		gnutls_deinit(pThis->sess);
	}
	RETiRet;
}


/* a small wrapper for gnutls_transport_set_ptr(). The main intension for
 * creating this wrapper is to get the annoying "cast to pointer from different
 * size" compiler warning just once. There seems to be no way around it, see:
 * http://lists.gnu.org/archive/html/help-gnutls/2008-05/msg00000.html
 * rgerhards, 2008.05-07
 */
static inline void
gtlsSetTransportPtr(nsd_gtls_t *pThis, int sock)
{
	/* Note: the compiler warning for the next line is OK - see header comment! */
	gnutls_transport_set_ptr(pThis->sess, (gnutls_transport_ptr_t) sock);
}

/* ---------------------------- end GnuTLS specifics ---------------------------- */


/* Standard-Constructor */
BEGINobjConstruct(nsd_gtls) /* be sure to specify the object type also in END macro! */
	iRet = nsd_ptcp.Construct(&pThis->pTcp);
	pThis->bReportAuthErr = 1;
CHKiRet(gtlsAddOurCert());
finalize_it:
ENDobjConstruct(nsd_gtls)


/* destructor for the nsd_gtls object */
BEGINobjDestruct(nsd_gtls) /* be sure to specify the object type also in END and CODESTART macros! */
CODESTARTobjDestruct(nsd_gtls)
	if(pThis->iMode == 1) {
		gtlsEndSess(pThis);
	}

	if(pThis->pTcp != NULL) {
		nsd_ptcp.Destruct(&pThis->pTcp);
	}
ENDobjDestruct(nsd_gtls)


/* Set the driver mode. For us, this has the following meaning:
 * 0 - work in plain tcp mode, without tls (e.g. before a STARTTLS)
 * 1 - work in TLS mode
 * rgerhards, 2008-04-28
 */
static rsRetVal
SetMode(nsd_t *pNsd, int mode)
{
	DEFiRet;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;

	ISOBJ_TYPE_assert((pThis), nsd_gtls);
	if(mode != 0 && mode != 1) {
		errmsg.LogError(NO_ERRCODE, "error: driver mode %d not supported by "
				"gtls netstream driver", mode);
		ABORT_FINALIZE(RS_RET_INVALID_DRVR_MODE);
	}

	pThis->iMode = mode;

finalize_it:
	RETiRet;
}


/* Set the authentication mode. For us, the following is supported:
 * anon - no certificate checks whatsoever (discouraged, but supported)
 * x509/fingerprint - certificate fingerprint
 * x509/name - cerfificate name check
 * mode == NULL is valid and defaults to x509/name
 * rgerhards, 2008-05-16
 */
static rsRetVal
SetAuthMode(nsd_t *pNsd, uchar *mode)
{
	DEFiRet;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;

	ISOBJ_TYPE_assert((pThis), nsd_gtls);
	if(mode == NULL || !strcasecmp((char*)mode, "x509/name")) {
		pThis->authMode = GTLS_AUTH_CERTNAME;
	} else if(!strcasecmp((char*) mode, "x509/fingerprint")) {
		pThis->authMode = GTLS_AUTH_CERTFINGERPRINT;
	} else if(!strcasecmp((char*) mode, "anon")) {
		pThis->authMode = GTLS_AUTH_CERTANON;
	} else {
		errmsg.LogError(NO_ERRCODE, "error: authentication mode '%s' not supported by "
				"gtls netstream driver", mode);
		ABORT_FINALIZE(RS_RET_VALUE_NOT_SUPPORTED);
	}

/* TODO: clear stored IDs! */

finalize_it:
dbgprintf("gtls auth mode %d set\n", pThis->authMode);
	RETiRet;
}


/* Set permitted peers. It is depending on the auth mode if this are 
 * fingerprints or names. -- rgerhards, 2008-05-19
 */
static rsRetVal
SetPermPeers(nsd_t *pNsd, permittedPeers_t *pPermPeers)
{
	DEFiRet;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;

	ISOBJ_TYPE_assert((pThis), nsd_gtls);
	if(pPermPeers == NULL)
		FINALIZE;

	if(pThis->authMode != GTLS_AUTH_CERTFINGERPRINT && pThis->authMode != GTLS_AUTH_CERTNAME) {
		errmsg.LogError(NO_ERRCODE, "authentication not supported by "
			"gtls netstream driver in the configured authentication mode - ignored");
		ABORT_FINALIZE(RS_RET_VALUE_NOT_IN_THIS_MODE);
	}

	pThis->pPermPeers = pPermPeers;

finalize_it:
	RETiRet;
}


/* Provide access to the underlying OS socket. This is primarily
 * useful for other drivers (like nsd_gtls) who utilize ourselfs
 * for some of their functionality. -- rgerhards, 2008-04-18
 */
static rsRetVal
SetSock(nsd_t *pNsd, int sock)
{
	DEFiRet;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;

	ISOBJ_TYPE_assert((pThis), nsd_gtls);
	assert(sock >= 0);

	nsd_ptcp.SetSock(pThis->pTcp, sock);

	RETiRet;
}


/* abort a connection. This is meant to be called immediately
 * before the Destruct call. -- rgerhards, 2008-03-24
 */
static rsRetVal
Abort(nsd_t *pNsd)
{
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;
	DEFiRet;

	ISOBJ_TYPE_assert((pThis), nsd_gtls);

	if(pThis->iMode == 0) {
		nsd_ptcp.Abort(pThis->pTcp);
	}

	RETiRet;
}



/* initialize the tcp socket for a listner
 * Here, we use the ptcp driver - because there is nothing special
 * at this point with GnuTLS. Things become special once we accept
 * a session, but not during listener setup.
 * gerhards, 2008-04-25
 */
static rsRetVal
LstnInit(netstrms_t *pNS, void *pUsr, rsRetVal(*fAddLstn)(void*,netstrm_t*),
	 uchar *pLstnPort, uchar *pLstnIP, int iSessMax)
{
	DEFiRet;
	CHKiRet(gtlsGlblInitLstn());
	iRet = nsd_ptcp.LstnInit(pNS, pUsr, fAddLstn, pLstnPort, pLstnIP, iSessMax);
finalize_it:
	RETiRet;
}


/* get the remote hostname. The returned hostname must be freed by the caller.
 * rgerhards, 2008-04-25
 */
static rsRetVal
GetRemoteHName(nsd_t *pNsd, uchar **ppszHName)
{
	DEFiRet;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;
	ISOBJ_TYPE_assert(pThis, nsd_gtls);
	iRet = nsd_ptcp.GetRemoteHName(pThis->pTcp, ppszHName);
	RETiRet;
}


/* get the remote host's IP address. The returned string must be freed by the
 * caller. -- rgerhards, 2008-04-25
 */
static rsRetVal
GetRemoteIP(nsd_t *pNsd, uchar **ppszIP)
{
	DEFiRet;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;
	ISOBJ_TYPE_assert(pThis, nsd_gtls);
	iRet = nsd_ptcp.GetRemoteIP(pThis->pTcp, ppszIP);
	RETiRet;
}


/* accept an incoming connection request - here, we do the usual accept
 * handling. TLS specific handling is done thereafter (and if we run in TLS
 * mode at this time).
 * rgerhards, 2008-04-25
 */
static rsRetVal
AcceptConnReq(nsd_t *pNsd, nsd_t **ppNew)
{
	DEFiRet;
	int gnuRet;
	nsd_gtls_t *pNew = NULL;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;

	ISOBJ_TYPE_assert((pThis), nsd_gtls);
	CHKiRet(nsd_gtlsConstruct(&pNew)); // TODO: prevent construct/destruct!
	CHKiRet(nsd_ptcp.Destruct(&pNew->pTcp));
	CHKiRet(nsd_ptcp.AcceptConnReq(pThis->pTcp, &pNew->pTcp));
	
	if(pThis->iMode == 0) {
		/* we are in non-TLS mode, so we are done */
		*ppNew = (nsd_t*) pNew;
		FINALIZE;
	}

	/* if we reach this point, we are in TLS mode */
	CHKiRet(gtlsInitSession(pNew));
	gtlsSetTransportPtr(pNew, ((nsd_ptcp_t*) (pNew->pTcp))->sock);
	pNew->authMode = pThis->authMode;
	pNew->pPermPeers = pThis->pPermPeers;

	/* we now do the handshake. This is a bit complicated, because we are 
	 * on non-blocking sockets. Usually, the handshake will not complete
	 * immediately, so that we need to retry it some time later.
	 */
	gnuRet = gnutls_handshake(pNew->sess);
	if(gnuRet == GNUTLS_E_AGAIN || gnuRet == GNUTLS_E_INTERRUPTED) {
		pNew->rtryCall = gtlsRtry_handshake;
		dbgprintf("GnuTLS handshake does not complete immediately - setting to retry (this is OK and normal)\n");
	} else if(gnuRet != 0) {
		ABORT_FINALIZE(RS_RET_TLS_HANDSHAKE_ERR);
	}

	pNew->iMode = 1; /* this session is now in TLS mode! */

	*ppNew = (nsd_t*) pNew;

finalize_it:
	if(iRet != RS_RET_OK) {
		if(pNew != NULL)
			nsd_gtlsDestruct(&pNew);
	}
	RETiRet;
}


/* receive data from a tcp socket
 * The lenBuf parameter must contain the max buffer size on entry and contains
 * the number of octets read (or -1 in case of error) on exit. This function
 * never blocks, not even when called on a blocking socket. That is important
 * for client sockets, which are set to block during send, but should not
 * block when trying to read data. If *pLenBuf is -1, an error occured and
 * errno holds the exact error cause.
 * rgerhards, 2008-03-17
 */
static rsRetVal
Rcv(nsd_t *pNsd, uchar *pBuf, ssize_t *pLenBuf)
{
	DEFiRet;
	ssize_t lenRcvd;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;
	ISOBJ_TYPE_assert(pThis, nsd_gtls);

	if(pThis->bAbortConn)
		ABORT_FINALIZE(RS_RET_CONNECTION_ABORTREQ);

	if(pThis->iMode == 0) {
		CHKiRet(nsd_ptcp.Rcv(pThis->pTcp, pBuf, pLenBuf));
		FINALIZE;
	}

	/* in TLS mode now */
	lenRcvd = gnutls_record_recv(pThis->sess, pBuf, *pLenBuf);
	if(lenRcvd < 0) {
		int gnuRet; /* TODO: build a specific function for GnuTLS error reporting */
		*pLenBuf = -1;
		CHKgnutls(lenRcvd); /* this will abort the function */
	}

	*pLenBuf = lenRcvd;

finalize_it:
	RETiRet;
}


/* send a buffer. On entry, pLenBuf contains the number of octets to
 * write. On exit, it contains the number of octets actually written.
 * If this number is lower than on entry, only a partial buffer has
 * been written.
 * rgerhards, 2008-03-19
 */
static rsRetVal
Send(nsd_t *pNsd, uchar *pBuf, ssize_t *pLenBuf)
{
	int iSent;
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;
	DEFiRet;
	ISOBJ_TYPE_assert(pThis, nsd_gtls);

	if(pThis->bAbortConn)
		ABORT_FINALIZE(RS_RET_CONNECTION_ABORTREQ);

	if(pThis->iMode == 0) {
		CHKiRet(nsd_ptcp.Send(pThis->pTcp, pBuf, pLenBuf));
		FINALIZE;
	}

	/* in TLS mode now */
	while(1) { /* loop broken inside */
		iSent = gnutls_record_send(pThis->sess, pBuf, *pLenBuf);
		if(iSent >= 0) {
			*pLenBuf = iSent;
			break;
		}
		if(iSent != GNUTLS_E_INTERRUPTED && iSent != GNUTLS_E_AGAIN) {
			dbgprintf("unexpected GnuTLS error %d in %s:%d\n", iSent, __FILE__, __LINE__);
			gnutls_perror(iSent); /* TODO: can we do better? */
			ABORT_FINALIZE(RS_RET_GNUTLS_ERR);
		}
	}

finalize_it:
	RETiRet;
}


/* open a connection to a remote host (server). With GnuTLS, we always
 * open a plain tcp socket and then, if in TLS mode, do a handshake on it.
 * rgerhards, 2008-03-19
 */
static rsRetVal
Connect(nsd_t *pNsd, int family, uchar *port, uchar *host)
{
	nsd_gtls_t *pThis = (nsd_gtls_t*) pNsd;
	int sock;
	int gnuRet;
	static const int cert_type_priority[3] = { GNUTLS_CRT_X509, GNUTLS_CRT_OPENPGP, 0 };
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, nsd_gtls);
	assert(port != NULL);
	assert(host != NULL);

	CHKiRet(nsd_ptcp.Connect(pThis->pTcp, family, port, host));

	if(pThis->iMode == 0)
		FINALIZE;
	
	/* we reach this point if in TLS mode */
	CHKgnutls(gnutls_init(&pThis->sess, GNUTLS_CLIENT));
	pThis->bHaveSess = 1;
	pThis->bIsInitiator = 1;

	/* Use default priorities */
	CHKgnutls(gnutls_set_default_priority(pThis->sess));
	CHKgnutls(gnutls_certificate_type_set_priority(pThis->sess, cert_type_priority));

	/* put the x509 credentials to the current session */
	CHKgnutls(gnutls_credentials_set(pThis->sess, GNUTLS_CRD_CERTIFICATE, xcred));

	/* assign the socket to GnuTls */
	CHKiRet(nsd_ptcp.GetSock(pThis->pTcp, &sock));
	gtlsSetTransportPtr(pThis, sock);

	/* and perform the handshake */
	CHKgnutls(gnutls_handshake(pThis->sess));
	dbgprintf("GnuTLS handshake succeeded\n");

	/* now check if the remote peer is permitted to talk to us */
	CHKiRet(gtlsChkFingerprint(pThis));

finalize_it:
	if(iRet != RS_RET_OK) {
		if(pThis->bHaveSess) {
			gnutls_deinit(pThis->sess);
			pThis->bHaveSess = 0;
		}
	}

	RETiRet;
}


/* queryInterface function */
BEGINobjQueryInterface(nsd_gtls)
CODESTARTobjQueryInterface(nsd_gtls)
	if(pIf->ifVersion != nsdCURR_IF_VERSION) {/* check for current version, increment on each change */
		ABORT_FINALIZE(RS_RET_INTERFACE_NOT_SUPPORTED);
	}

	/* ok, we have the right interface, so let's fill it
	 * Please note that we may also do some backwards-compatibility
	 * work here (if we can support an older interface version - that,
	 * of course, also affects the "if" above).
	 */
	pIf->Construct = (rsRetVal(*)(nsd_t**)) nsd_gtlsConstruct;
	pIf->Destruct = (rsRetVal(*)(nsd_t**)) nsd_gtlsDestruct;
	pIf->Abort = Abort;
	pIf->LstnInit = LstnInit;
	pIf->AcceptConnReq = AcceptConnReq;
	pIf->Rcv = Rcv;
	pIf->Send = Send;
	pIf->Connect = Connect;
	pIf->SetSock = SetSock;
	pIf->SetMode = SetMode;
	pIf->SetAuthMode = SetAuthMode;
	pIf->SetPermPeers =SetPermPeers;
	pIf->GetRemoteHName = GetRemoteHName;
	pIf->GetRemoteIP = GetRemoteIP;
finalize_it:
ENDobjQueryInterface(nsd_gtls)


/* exit our class
 */
BEGINObjClassExit(nsd_gtls, OBJ_IS_LOADABLE_MODULE) /* CHANGE class also in END MACRO! */
CODESTARTObjClassExit(nsd_gtls)
	gtlsGlblExit();	/* shut down GnuTLS */

	/* release objects we no longer need */
	objRelease(nsd_ptcp, LM_NSD_PTCP_FILENAME);
	objRelease(glbl, CORE_COMPONENT);
	objRelease(errmsg, CORE_COMPONENT);
ENDObjClassExit(nsd_gtls)


/* Initialize the nsd_gtls class. Must be called as the very first method
 * before anything else is called inside this class.
 * rgerhards, 2008-02-19
 */
BEGINObjClassInit(nsd_gtls, 1, OBJ_IS_LOADABLE_MODULE) /* class, version */
	/* request objects we use */
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(nsd_ptcp, LM_NSD_PTCP_FILENAME));

	/* now do global TLS init stuff */
	CHKiRet(gtlsGlblInit());
ENDObjClassInit(nsd_gtls)


/* --------------- here now comes the plumbing that makes as a library module --------------- */


BEGINmodExit
CODESTARTmodExit
	nsdsel_gtlsClassExit();
	nsd_gtlsClassExit();
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_LIB_QUERIES
ENDqueryEtryPt


BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */

	/* Initialize all classes that are in our module - this includes ourselfs */
	CHKiRet(nsd_gtlsClassInit(pModInfo)); /* must be done after tcps_sess, as we use it */
	CHKiRet(nsdsel_gtlsClassInit(pModInfo)); /* must be done after tcps_sess, as we use it */

ENDmodInit
/* vi:set ai:
 */
