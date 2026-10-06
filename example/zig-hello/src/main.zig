const std = @import("std");
const cwist = @import("cwist");

fn hello(_: void, _: cwist.Request, res: cwist.Response) void {
    res.setStatus(200);
    res.addHeader("X-Powered-By", "CWIST Zig") catch {};
    res.setBody("Hello, World!") catch {};
}

fn user(_: void, req: cwist.Request, res: cwist.Response) void {
    var buf: [128]u8 = undefined;
    const body = std.fmt.bufPrint(&buf, "user {s}", .{req.param("id") orelse "?"}) catch "user ?";
    res.setStatus(200);
    res.addHeader("X-Powered-By", "CWIST Zig") catch {};
    res.setBody(body) catch {};
}

pub fn main() !void {
    var app = try cwist.App.init();
    defer app.deinit();

    try app.get("/", {}, hello);
    try app.get("/users/:id", {}, user);

    std.debug.print("Listening on http://127.0.0.1:8080 (press Ctrl-C to stop)\n", .{});
    try app.listen(8080);
}
