// Regression: __attribute__((...)) on an enum member rejected with
// "expected ','". libcurl decorates enumerators with CURL_DEPRECATED(...);
// affected git http.c, http-fetch.c, remote-curl.c.
//
// Fix: in enum_specifier(), call attribute_list() between the enumerator name
// and the optional `= value` so enumerator-attached attributes are consumed.
//
// Fixed: 2026-04-29.

enum E {
  E_OLD __attribute__((deprecated)) = 1,
  E_OLD2 __attribute__((deprecated("use E_NEW"))),
  E_NEW = 3,
  E_LAST __attribute__((unused))
};

int main(void) { return E_NEW; }
