# Records

One key, one value. These are the three operations most clients will ever use.

## Keys and values

A key is a string and a value is a string, and **every string is valid**. The
service does not parse a value, does not require a schema and does not reserve
any key: what is written is what is read back.

- **Keys** are percent-encoded in the path, and decode to UTF-8. Every string
  is a key except the empty one, which has no path of its own —
  `/table/account/key/` is the [scan](/database/scans) endpoint. A key is one
  part or [two](#partition-keys-and-sort-keys).
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

## Partition keys and sort keys

A key is a **partition key** and, optionally, a **sort key**, and the path
carries them as two segments:

```http
PUT /table/transaction/key/4821/2026-09-10T09:14:22Z
```

The two decide different things:

- The **partition key** decides *where* the record lives — which
  [partition](/database/cluster#partitions) it is in, and so which node in each
  zone holds it, leads its writes and moves it when the membership changes. It
  is the only half the cluster hashes.
- The **sort key** decides *where it sits* among the records that share that
  partition key. It moves nothing.

So every record of one partition key is held by one node, in sort key order,
however many of them there are. That is what the split is for: one account's
transactions are one node's to scan, and a record and the records derived from
it are one hop rather than a fan-out.

It is also what the split costs. A partition key is the unit the cluster
balances, so **one partition key is never spread over two nodes**: a partition
key with far more records under it than the others is a node with more of the
table than the others, and no membership change will even it out.

Both halves are percent-encoded, and the slash between them is the only one
that is not: a slash *inside* either half is `%2F`, as it always was.

```http
GET /table/transaction/key/4821/2026-09-10T09%3A14%3A22Z
```

### One key, written two ways

The two halves are one key with a **zero byte** between them, and it is the
first zero byte that separates. So these name the same record:

```http
GET /table/transaction/key/4821/2019
GET /table/transaction/key/4821%002019
```

**The slash is the one to send.** A path carrying a zero byte is refused by the
nginx in front of every instance, with a `400` of its own and no JSON body,
before the database is asked — its URI parser rejects the byte and no directive
turns that off. The encoded form is therefore what reaches a node addressed on
its [API port](/database/cluster#turning-it-on) directly, which is how one node
writes a composed key when it forwards to another. Through the proxy, a
composed key is spelled with the slash.

Three things follow, and they are the whole of the rule:

- **A key of one part is a partition key with no sort key.** Every key that
  carries no zero byte is that, so a table written without ever thinking about
  sort keys behaves exactly as it did.
- **A partition key cannot contain a zero byte**, because the first one is the
  separator. A sort key can: only the first separates — though the path that
  writes one is the encoded form, so it is a key the proxy will not carry.
- **The 4 KiB limit is over the whole key** — both halves and the byte between
  them.

`4821` and `4821/2019` are different records, and neither is a container for
the other. What they share is the node that holds them.

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

`204 No Content`, and a key that was already there is overwritten.

There is no `POST` to a table and no server-generated key. Keys carry meaning in
an ordered store — they decide what a scan can answer — so the API will not
invent one.

## There is no way to erase one key

`DELETE` on a key is `405 method_not_allowed`, and so is `DELETE` on a range of
them. **The only thing that erases a record is
[deleting its table](/database/tables#delete-a-table)**, which takes every
record in it.

A value that should no longer be read is overwritten rather than removed —
with the empty string, or with whatever a client reads as absent. The key stays,
and a scan still answers it.
