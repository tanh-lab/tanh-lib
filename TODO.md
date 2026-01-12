TODOs

- [ ] Fix the update from json method and the param to json method.
- [ ] Add an in_gesture option to the state
- [ ] How to register threads without beeing on them? For rcu and buffer registration! So that we don't have a single real-time violation on audio threads.
- [ ] Think about the buffered strings implementation in the path resolution. Is there no more elegant way? At the moment when we set a new parameter in the state within a callback of another parameter, the string_view buffers get corrupted. Also we actually do not need any buffered string_views at all in the set method as it is not called from real-time contexts. So maybe we can have a non-buffered version of the path resolution for the set method, that fixes the nested call corruption issue. See the uncommented test in the state_tests.
