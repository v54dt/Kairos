@0x99545b8926effaa8;

# Kairos quote wire schema.

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
  quoteTsUs  @2 :Int64;
  bids       @3 :List(PriceLevel);
  asks       @4 :List(PriceLevel);
  lastPrice  @5 :Int64;
  lastScale  @6 :UInt8;
  lastVolume @7 :Int64;
  isTrial    @8 :Bool;
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

struct Envelope {
  union {
    quote       @0 :Quote;
    subscribe   @1 :Subscribe;
    unsubscribe @2 :Unsubscribe;
    subAck      @3 :SubAck;
    error       @4 :Text;
    heartbeat   @5 :Void;
  }
}
