// Only the two modules the production matcher uses are vendored. The engine's
// MEStateMachine (engine/src/rpc/me_state_machine.rs) constructs
// optimised_fifo::FIFOBook, so that is the book under test. The art_book and
// (heap) fifo variants are not the production book and pull an external git
// dependency / are not used by the server, so they are omitted here.
pub mod book;
pub mod optimised_fifo;
