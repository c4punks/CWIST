use cwist::App;

fn main() {
    let mut app = App::new().expect("create CWIST app");

    app.get("/", |_req, res| {
        res.set_status(200);
        let _ = res.set_body("Hello, World!");
    })
    .expect("register /");

    app.get("/users/:id", |req, res| {
        let id = req.param("id").unwrap_or("?");
        res.set_status(200);
        let _ = res.set_body(format!("user {id}"));
    })
    .expect("register /users/:id");

    app.use_middleware(|_req, res, next| {
        res.add_header("X-Powered-By", "CWIST Rust").expect("header");
        next();
    });

    println!("Listening on http://127.0.0.1:8080 (press Ctrl-C to stop)");
    app.listen(8080).expect("listen");
}
