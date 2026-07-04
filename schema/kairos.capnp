@0x99545b8926effaa8;

# Kairos quote wire schema.

enum Exchange {
  twse @0;
  tpex @1;
  tfx @2;
  otc @3;
}

# Quote-side board marker. `unknown @0` is deliberate so old records (which lack
# this field and decode to @0) are not mislabelled. Distinct from order-wire
# `Board` (whose @0 is oddLot); appending `unknown` there would mutate the frozen
# order-wire section.
enum QuoteBoard {
  unknown  @0;
  roundLot @1;
  oddLot   @2;
}

# Trading session for futures/night market. `unknown @0` keeps equity/legacy
# records honest; E1/E2 populate it once the futures feed lands.
enum Session {
  unknown @0;
  day     @1;
  night   @2;
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
  # lastPrice/lastScale/lastVolume are a best-effort last-trade snapshot kept for
  # back-compat (old recordings + current exec consumers). Authoritative trades
  # are `Trade` events. Never remove/renumber these ordinals.
  lastPrice  @5 :Int64;
  lastScale  @6 :UInt8;
  lastVolume @7 :Int64;
  isTrial    @8 :Bool;
  # A2 append-only additions (default 0/unknown -> old wire bytes truncate identically).
  source          @9  :UInt16;     # feed origin: 0=concords, 1=fubon, 2=shioaji (reserved)
  seq             @10 :UInt64;     # per-(source,symbol) monotonic; 0=legacy/no-seq
  epoch           @11 :UInt32;     # feed-session generation; 0=legacy/no-epoch
  recvTsUs        @12 :Int64;      # sidecar receive time, CLOCK_REALTIME us; 0=legacy
  board           @13 :QuoteBoard;
  # E2 futures reservation (added now, populated by E1/E2).
  session         @14 :Session;
  tradingDate     @15 :UInt32;     # yyyymmdd, exchange trading date
  simtrade        @16 :Bool;       # futures simulation session flag
  underlyingPrice @17 :Int64;      # underlying price mantissa (futures/options)
}

# A distinct TRADE event, separate from the DEPTH-bearing Quote. fubon/shioaji
# deliver book and trade natively separate; concords delivers them combined and
# the sidecar splits one Quotation into a Quote plus (when a trade is present) a
# Trade. Offline fill simulation / hftbacktest (A4/A5) need both event types, and
# a lastPrice==0-as-no-trade dual meaning would pollute the queue model.
struct Trade {
  symbol          @0  :Text;
  exchange        @1  :Exchange;
  source          @2  :UInt16;
  seq             @3  :UInt64;
  epoch           @4  :UInt32;
  tradeTsUs       @5  :Int64;      # broker trade timestamp, us
  recvTsUs        @6  :Int64;      # sidecar receive time, CLOCK_REALTIME us
  priceMantissa   @7  :Int64;
  priceScale      @8  :UInt8;
  volume          @9  :Int64;
  isTrial         @10 :Bool;       # equity 試撮 (concords IsTrial)
  # E2 futures reservation.
  session         @11 :Session;
  tradingDate     @12 :UInt32;
  simtrade        @13 :Bool;       # futures simulation session flag
  underlyingPrice @14 :Int64;
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
    trade       @6 :Trade;
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
