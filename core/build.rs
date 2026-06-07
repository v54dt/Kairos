fn main() {
    capnpc::CompilerCommand::new()
        .src_prefix("../schema")
        .file("../schema/kairos.capnp")
        .run()
        .expect("compiling kairos.capnp");
    println!("cargo:rerun-if-changed=../schema/kairos.capnp");
}
