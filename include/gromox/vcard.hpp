#pragma once
#include <string>
#include <utility>
#include <vector>
#include <gromox/common_types.hpp>
#include <gromox/mapierr.hpp>
#include <gromox/double_list.hpp>

struct GX_EXPORT vcard_param {
	vcard_param(const char *);
	ec_error_t append_paramval(const char *paramval);
	inline const char *name() const { return m_name.c_str(); }

	std::string m_name;
	std::vector<std::string> m_paramvals;
};
using VCARD_PARAM = vcard_param;

struct GX_EXPORT vcard_value {
	ec_error_t append_subval(const char *);
	std::vector<std::string> m_subvals;
};
using VCARD_VALUE = vcard_value;

struct GX_EXPORT vcard_line {
	vcard_line() = default;
	vcard_line(const char *);
	inline vcard_param &append_param(vcard_param &&o) { m_params.push_back(std::move(o)); return m_params.back(); }
	vcard_param &append_param(const char *p, const char *pv);
	inline vcard_value &append_value(vcard_value &&o) { m_values.push_back(std::move(o)); return m_values.back(); }
	inline vcard_value &append_value() { return m_values.emplace_back(); }
	vcard_value &append_value(const char *);
	const char *get_first_subval() const;
	inline const char *name() const { return m_name.c_str(); }

	DOUBLE_LIST_NODE node{};
	std::string m_name;
	std::vector<vcard_param> m_params;
	std::vector<vcard_value> m_values;
};
using VCARD_LINE = vcard_line;

struct GX_EXPORT vcard {
	vcard();
	vcard(vcard &&o);
	~vcard();
	vcard &operator=(vcard &&);

	void clear();
	ec_error_t retrieve_single(char *in_buff);
	BOOL serialize(char *out_buff, size_t max_length);
	ec_error_t append_line(vcard_line *);
	vcard_line &append_line(const char *);
	vcard_line &append_line(const char *, const char *);

	DOUBLE_LIST line_list{};
};
using VCARD = vcard;

VCARD_LINE* vcard_new_line(const char *name);
extern GX_EXPORT ec_error_t vcard_retrieve_multi(char *input, std::vector<vcard> &, size_t limit = 0);
