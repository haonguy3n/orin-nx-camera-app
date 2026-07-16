/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Mimic of folly/Unit.h (github.com/facebook/folly): the regular void —
 * a value type usable where `void` isn't (e.g. Expected<Unit, Error>).
 * Dropped: the Lift/Drop metafunctions.
 */
#pragma once

namespace folly {

struct Unit {
    constexpr bool operator==(const Unit& /*other*/) const { return true; }
    constexpr bool operator!=(const Unit& /*other*/) const { return false; }
};

constexpr Unit unit{};

}  // namespace folly
