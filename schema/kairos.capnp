@0x99545b8926effaa8;

# Kairos quote wire schema. Shared by the Rust core and the C++ concords sidecar,
# carried over both Aeron (sidecar -> core) and the UDS feed (core -> consumers).
# Prices are mantissa + scale (lossless from concords FixedPoint{digits,precision});
# core never does decimal math, so integers travel end to end.

enum Exchange {
  twse @0;
  tpex @1;
  tfx @2;
  otc @3;
}

struct PriceLevel {
  priceMantissa @0 :Int64;
  priceScale    @1 :UInt8;
  volume        @2 :Int64;
}

struct Quote {
  symbol     @0 :Text;
  exchange   @1 :Exchange;
  quoteTsUs  @2 :Int64;             # concords timestamp, micros since epoch
  bids       @3 :List(PriceLevel);  # up to 5, best first
  asks       @4 :List(PriceLevel);  # up to 5, best first
  lastPrice  @5 :Int64;             # last trade, mantissa
  lastScale  @6 :UInt8;
  lastVolume @7 :Int64;
  isTrial    @8 :Bool;              # 試撮
}

struct Subscribe {
  symbols @0 :List(Text);
}

struct Unsubscribe {
  symbols @0 :List(Text);
}

struct SubAck {
  symbols @0 :List(Text);
  ok      @1 :Bool;
}

# One envelope covers both directions; the union discriminant is the message tag.
struct Envelope {
  union {
    quote       @0 :Quote;        # core -> client (data)
    subscribe   @1 :Subscribe;    # client -> core (control)
    unsubscribe @2 :Unsubscribe;  # client -> core (control)
    subAck      @3 :SubAck;       # core -> client
    error       @4 :Text;         # core -> client
    heartbeat   @5 :Void;
  }
}
