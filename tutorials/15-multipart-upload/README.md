# Tutorial 15: Multipart File Upload

Parse `multipart/form-data` request bodies and save uploaded file streams to disk.

## Key Concepts
- Extracting the boundary with `cwist_multipart_extract_boundary(content_type)`.
- Parsing the body with `cwist_multipart_parse(body, body_len, boundary)`.
- Freeing parse results with `cwist_multipart_result_destroy(result)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut15
```
