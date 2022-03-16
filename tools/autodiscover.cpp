// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2022 grommunio GmbH
// This file is part of Gromox.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <tinyxml2.h>
#include <curl/curl.h>
#include <libHX/option.h>
#include <gromox/scope.hpp>

struct curl_del {
	void operator()(CURL *x) const { curl_easy_cleanup(x); }
	void operator()(curl_slist *x) const { curl_slist_free_all(x); }
};

using namespace std::string_literals;
using namespace gromox;

static char *g_disc_host, *g_disc_url, *g_emailaddr, *g_password, *g_legacydn;
static constexpr HXoption g_options_table[] = {
	{nullptr, 'h', HXTYPE_STRING, &g_disc_host, nullptr, nullptr, 0, "Host to contact (in absence of -H; default: localhost)"},
	{nullptr, 'H', HXTYPE_STRING, &g_disc_url, nullptr, nullptr, 0, "Full autodiscover URL to use"},
	{nullptr, 'e', HXTYPE_STRING, &g_emailaddr, nullptr, nullptr, 0, "E-mail address for user lookup"},
	{nullptr, 'p', HXTYPE_STRING, &g_password, nullptr, nullptr, 0, "Use the $PASS environment variable for password"},
	{nullptr, 'x', HXTYPE_STRING, &g_legacydn, nullptr, nullptr, 0, "Legacy DN"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static tinyxml2::XMLElement *adc(tinyxml2::XMLElement *e,
    const char *tag, const char *val = nullptr)
{
	auto ch = e->InsertNewChildElement(tag);
	if (val != nullptr)
		ch->SetText(val);
	return ch;
}

static std::unique_ptr<tinyxml2::XMLPrinter>
oxd_make_request(const char *email, const char *dn)
{
	tinyxml2::XMLDocument doc;
	doc.NewDeclaration();
	auto root = doc.NewElement("Autodiscover");
	doc.InsertFirstChild(root);
	root->SetAttribute("xmlns", "http://schemas.microsoft.com/exchange/autodiscover/outlook/requestschema/2006");
	auto req = adc(root, "Request");
	if (email != nullptr)
		adc(req, "EMailAddress", email);
	if (dn != nullptr)
		adc(req, "LegacyDN", dn);
	adc(req, "AcceptableResponseSchema", "http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a");

	auto prt = std::make_unique<tinyxml2::XMLPrinter>(nullptr, false, 0);
	doc.Print(prt.get());
	return prt;
	/* tinyxml has no move ctors and no copy ctors, ugh */
}

static size_t oxd_write(char *ptr, size_t size, size_t nemb, void *udata)
{
	static_cast<std::string *>(udata)->append(ptr, size * nemb);
	return size * nemb;
}

static CURLcode setopts(CURL *ch, const char *password, curl_slist *hdrs,
    tinyxml2::XMLPrinter &xml_request, std::string &xml_response)
{
	auto ret = curl_easy_setopt(ch, CURLOPT_NOPROGRESS, 1L);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1L);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_TCP_NODELAY, 0L);
	if (ret != CURLE_OK)
		return ret;
	ret= curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 0L);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 0L);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1L);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_USERNAME, g_emailaddr != nullptr ?
	      g_emailaddr : g_legacydn);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_PASSWORD, password);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_HTTPHEADER, hdrs);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_POSTFIELDSIZE_LARGE, xml_request.CStrSize());
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_POSTFIELDS, xml_request.CStr());
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_WRITEDATA, &xml_response);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, oxd_write);
	if (ret != CURLE_OK)
		return ret;
	ret = curl_easy_setopt(ch, CURLOPT_VERBOSE, 1L);
	if (ret != CURLE_OK)
		return ret;
	if (g_disc_url != nullptr)
		ret = curl_easy_setopt(ch, CURLOPT_URL, g_disc_url);
	else if (g_disc_host != nullptr)
		ret = curl_easy_setopt(ch, CURLOPT_URL, ("https://"s +
		      g_disc_host + "/Autodiscover/Autodiscover.xml").c_str());
	else
		ret = curl_easy_setopt(ch, CURLOPT_URL,
		      "https://localhost/Autodiscover/Autodiscover.xml");
	if (ret != CURLE_OK)
		return ret;
	return CURLE_OK;
}

int main(int argc, const char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
	auto cl_args = make_scope_exit([]() {
		free(g_disc_host);
		free(g_disc_url);
		free(g_emailaddr);
		free(g_password);
		free(g_legacydn);
	});
	if (g_disc_url != nullptr && g_disc_host != nullptr) {
		fprintf(stderr, "Can only use one of -H and -h.\n");
		return EXIT_FAILURE;
	}
	auto password = g_password != nullptr ? g_password : getenv("PASS");
	if (password == nullptr) {
		fprintf(stderr, "No password specified. Use the $PASS environment variable.\n");
		return EXIT_FAILURE;
	} else if (g_emailaddr == nullptr && g_legacydn == nullptr) {
		fprintf(stderr, "At least one of -e or -x is required.\n");
		return EXIT_FAILURE;
	}

	auto xml_request = oxd_make_request(g_emailaddr, g_legacydn);
	std::string xml_response;
	std::unique_ptr<CURL, curl_del> chp(curl_easy_init());
	std::unique_ptr<curl_slist, curl_del> hdrs(curl_slist_append(nullptr, "Content-Type: text/xml"));
	auto ch = chp.get();
	auto result = setopts(ch, password, hdrs.get(), *xml_request, xml_response);
	if (result != CURLE_OK) {
		fprintf(stderr, "curl_easy_setopt(): %s\n", curl_easy_strerror(result));
		return EXIT_FAILURE;
	}
	printf("* Request body:\n%s\n\n", xml_request->CStr());
	result = curl_easy_perform(ch);
	if (result != CURLE_OK) {
		fprintf(stderr, "curl_easy_perform(): %s\n", curl_easy_strerror(result));
		return EXIT_FAILURE;
	}
	printf("* Response body:\n%s\n", xml_response.c_str());
	return EXIT_SUCCESS;
}