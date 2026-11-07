/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file pool_generation.cpp Test the per-lifetime generation of pool items. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../core/pool_func.hpp"

#include "../safeguards.h"

/** Index type of the pool used by these tests. */
using TestPoolID = PoolID<uint16_t, struct TestPoolIDTag, 16, 0xFFFF>;

struct TestPoolItem;
using TestPool = Pool<TestPoolItem, TestPoolID, 4>;
extern TestPool _test_pool;

/** A pool item that only exists for these tests. */
struct TestPoolItem : TestPool::PoolItem<&_test_pool> {
	TestPoolItem(TestPoolID index) : TestPool::PoolItem<&_test_pool>(index) {}
};

TestPool _test_pool("test_pool_generation");

TEST_CASE("Pool - generation distinguishes a recreated item")
{
	REQUIRE(TestPoolItem::CanAllocateItem());
	TestPoolItem *first = TestPoolItem::Create();
	REQUIRE(first != nullptr);
	const TestPoolID index = first->index;
	const uint64_t generation = first->generation;
	CHECK(generation != 0);

	delete first;

	REQUIRE(TestPoolItem::CanAllocateItem());
	TestPoolItem *second = TestPoolItem::Create();
	REQUIRE(second != nullptr);
	/* The pool reuses the freed index, but the item is a new lifetime. */
	CHECK(second->index == index);
	CHECK(second->generation != generation);

	delete second;
}

TEST_CASE("Pool - generation is unique among live items")
{
	REQUIRE(TestPoolItem::CanAllocateItem(2));
	TestPoolItem *a = TestPoolItem::Create();
	TestPoolItem *b = TestPoolItem::Create();
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	CHECK(a->generation != b->generation);

	delete a;
	delete b;
}
