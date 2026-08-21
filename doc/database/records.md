# Records

One key, one value. These are the four operations most clients will ever use.

## Keys and values

A key is a string and a value is a string, and **every string is valid**. The
service does not parse a value, does not require a schema and does not reserve
any key: what is written is what is read back.

- **Keys** are percent-encoded in the path, and decode to UTF-8. Every string
  is a key except the empty one, which has no path of its own —
  `/table/account/key/` is the [scan](/database/scans) endpoint.
- **Values** are the body of the request and the body of the response, sent as
  UTF-8. The empty string is a value like any other, and is not the same thing
  as a missing key.

A value that happens to be a JSON document is stored and returned like any other
string. That is the common case, and it is the client that gives it meaning:

```http
PUT /table/account/key/4821
Content-Type: application/json

{"firstName":"Eleanor","lastName":"Whitmore"}
```

The service keeps the bytes, not the structure. It will not reject a value for
being malformed JSON, because it never looked.

Only the [size limits](/database/reference#limits) constrain what a string may
be — 4 KiB of key and 16 MiB of value, counted as UTF-8 bytes. A key that does
not percent-decode to valid UTF-8 is `400 invalid_key_encoding`, which is a
statement about the path, not about the key: it never was a string.

## Read a record

```http
GET /table/account/key/4821
```

```http
200 OK
Content-Type: text/plain; charset=utf-8

Eleanor Whitmore
```

The body is the value, and nothing else: there is no envelope to unwrap.

`404 Not Found` when there is no such key, with no body. A missing key and an
empty value are different things, and the status code is what tells them apart.

`HEAD` answers the same headers with no body, and is the cheap way to ask
whether a key exists and how large it is.

## Write a record

```http
PUT /table/account/key/4821
Content-Type: text/plain; charset=utf-8

Eleanor Whitmore
```

The body is the value. The `Content-Type` is the client's business — it is not
stored, and it does not change how the value is treated.

There is no `POST` to a table and no server-generated key. Keys carry meaning in
an ordered store — they decide what a scan can answer — so the API will not
invent one.

## Delete a record

```http
DELETE /table/account/key/4821
```
