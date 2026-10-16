# Good and Bad Tests

## Good Tests

**Integration-style**: Test through real interfaces, not mocks of internal parts.

The snippets use illustrative C++ types, not repository APIs.

```cpp
TEST(CheckoutTest, validCartProducesConfirmedOrder) {
  Cart cart;
  cart.add(Product{.price = 10});

  const auto result = checkout(cart);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, OrderStatus::confirmed);
}
```

Characteristics:

- Tests behavior users/callers care about
- Uses public API only
- Survives internal refactors
- Describes WHAT, not HOW
- One logical assertion per test

## Bad Tests

**Implementation-detail tests**: Coupled to internal structure.

```cpp
TEST(CheckoutTest, callsPaymentProcessorOnce) {
  CountingPaymentProcessor processor;
  (void)checkout(cart, processor);
  EXPECT_EQ(processor.callCount(), 1);  // implementation detail
}
```

Red flags:

- Mocking internal collaborators
- Testing private methods
- Asserting on call counts/order
- Test breaks when refactoring without behavior change
- Test name describes HOW not WHAT
- Verifying through external means instead of interface

```cpp
TEST(UserStoreTest, createUserWritesExpectedRow) {
  const auto user = createUser(UserDraft{.name = "Alice"});
  EXPECT_TRUE(rawDatabaseQuery(user.id).has_value());  // bypasses public seam
}

TEST(UserStoreTest, createdUserIsRetrievable) {
  const auto user = createUser(UserDraft{.name = "Alice"});
  const auto retrieved = findUser(user.id);
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->name, "Alice");
}
```

**Tautological tests**: Expected value restates the implementation, so the test passes by construction.

```cpp
TEST(CartTest, calculatesTotal) {
  const auto items = std::vector{Item{.price = 10}, Item{.price = 5}};
  const auto expected = items[0].price + items[1].price;
  EXPECT_EQ(calculateTotal(items), expected);  // repeats the implementation
}

TEST(CartTest, twoKnownPricesProduceFifteen) {
  const auto items = std::vector{Item{.price = 10}, Item{.price = 5}};
  EXPECT_EQ(calculateTotal(items), 15);
}
```
