TODOs

- [ ] With RT-San fix the tests that throw exceptions (SmartHandle.ThrowsOnNonexistentParameter, StateTests.HandleNonExistentKey)
- [ ] Properly disable HardwareTests on plattforms that are not supported
- [ ] Add on_gesture_start and on_gesture_end callbacks for parameter_listners so it gets notified when the gesture starts and ends. 
- [ ] The parameter listeners shall also have a notification strategy, so that we don't have to specify it every time we set a parameter only we need to specify the source (listener). It might be even better to not have the ability to specify the strategy on set, but only on listener registration. So when we set a parameter, the listeners get notified according the strategy of the source listener. When no source listener is specified, the default strategy is used (notify all that want to be notified). This simplifies the set method and makes the notification strategy more consistent.
- [ ] Callback listeners shall have no option for notification strategy or in_gesture, they always get notified.
- [ ] The ModulationMatrix shall also be able to modulate bools or ints or doubles
- [ ] Check that RCU still works after setting the min_active_period to current_period in the cleanup_safe_versions method (no more to UINT64_MAX)