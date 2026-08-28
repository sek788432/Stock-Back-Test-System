# When to Mock

Use a fake or mock only at a real system boundary:

- External provider APIs
- Databases (prefer a temporary real database when practical)
- Time/randomness
- File system (sometimes)

Don't mock:

- Concrete project classes or internal collaborators
- Pure project logic that can run directly in the test
- A boundary merely because its implementation is inconvenient

A project-owned port may be faked when its responsibility is to isolate a real
external boundary. Test the production adapter separately against that boundary
when practical.

## Designing for Mockability

Do not create an interface solely for a test. When production already owns a
boundary seam, inject that narrow seam. The snippets below are illustrative C++
and do not declare repository APIs.

**1. Use dependency injection**

Pass external dependencies in rather than creating them internally:

```cpp
class PaymentClient {
public:
  virtual ~PaymentClient() = default;
  [[nodiscard]] virtual ChargeResult charge(Money amount) = 0;
};

[[nodiscard]] ChargeResult processPayment(const Order& order,
                                          PaymentClient& client) {
  return client.charge(order.total);
}
```

**2. Prefer operation-shaped interfaces over generic request dispatch**

Create specific functions for each external operation instead of one generic function with conditional logic:

```cpp
class OrderProvider {
public:
  virtual ~OrderProvider() = default;
  [[nodiscard]] virtual Result<User> findUser(UserId id) = 0;
  [[nodiscard]] virtual Result<std::vector<Order>> findOrders(UserId id) = 0;
};

// Avoid a generic request(std::string_view operation, Payload payload) seam
// that forces every fake to reimplement dispatch and untyped result decoding.
```

The operation-shaped approach means:
- Each mock returns one specific shape
- No conditional logic in test setup
- Easier to see which operations a test exercises
- Type safety per operation
