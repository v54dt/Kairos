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

# --- Order wire (scenario <-> concords order hub, separate UDS) ---

enum Market { tse @0; otc @1; }
enum Side   { buy @0; sell @1; }
enum Board  { oddLot @0; roundLot @1; }

struct OrderSubmit {
  id          @0 :Text;    # user_defined_id (k<pid>-<seq>)
  symbol      @1 :Text;
  market      @2 :Market;
  board       @3 :Board;
  side        @4 :Side;
  fundingType @5 :Text;    # parsed hub-side
  timeInForce @6 :Text;    # parsed hub-side
  priceCents  @7 :Int64;   # integer cents, formatted hub-side
  shares      @8 :Int64;   # raw shares; hub converts by board
}

struct OrderCancel {
  id @0 :Text;
}

struct OrderAck {
  id           @0 :Text;
  ok           @1 :Bool;
  errorMessage @2 :Text;
}

struct OrderFill {
  id         @0 :Text;
  shares     @1 :Int64;
  priceCents @2 :Int64;
}

struct OrderCancelResult {
  id @0 :Text;
  ok @1 :Bool;
}

struct OrderEnvelope {
  union {
    submit       @0 :OrderSubmit;
    cancel       @1 :OrderCancel;
    ack          @2 :OrderAck;
    fill         @3 :OrderFill;
    cancelResult @4 :OrderCancelResult;
    heartbeat    @5 :Void;
  }
}
