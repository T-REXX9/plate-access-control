from __future__ import annotations

import csv
import io
import math
import os
import re
import secrets
import sqlite3
from functools import wraps
from pathlib import Path
from typing import Any

from flask import (
    Flask,
    Response,
    abort,
    flash,
    g,
    redirect,
    render_template,
    request,
    send_file,
    session,
    url_for,
)
from werkzeug.security import check_password_hash, generate_password_hash


PROJECT_DIR = Path(__file__).resolve().parent.parent
DATABASE_DIR = PROJECT_DIR / "database"
DATABASE_PATH = DATABASE_DIR / "gate_access.db"
SCHEMA_PATH = DATABASE_DIR / "schema.sql"
SECRET_PATH = DATABASE_DIR / "web_secret.key"


def load_secret_key() -> str:
    DATABASE_DIR.mkdir(parents=True, exist_ok=True)
    if not SECRET_PATH.exists():
        SECRET_PATH.write_text(secrets.token_hex(32), encoding="utf-8")
        SECRET_PATH.chmod(0o600)
    return SECRET_PATH.read_text(encoding="utf-8").strip()


app = Flask(__name__)
app.config.update(
    SECRET_KEY=load_secret_key(),
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Lax",
    PERMANENT_SESSION_LIFETIME=60 * 60 * 8,
    MAX_CONTENT_LENGTH=2 * 1024 * 1024,
)


def initialize_database() -> None:
    DATABASE_DIR.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(DATABASE_PATH)
    connection.executescript(SCHEMA_PATH.read_text(encoding="utf-8"))
    connection.close()


initialize_database()


def get_db() -> sqlite3.Connection:
    if "db" not in g:
        g.db = sqlite3.connect(DATABASE_PATH, timeout=10)
        g.db.row_factory = sqlite3.Row
        g.db.execute("PRAGMA foreign_keys = ON")
        g.db.execute("PRAGMA busy_timeout = 10000")
    return g.db


@app.teardown_appcontext
def close_db(_: BaseException | None) -> None:
    connection = g.pop("db", None)
    if connection is not None:
        connection.close()


def admin_exists() -> bool:
    row = get_db().execute("SELECT 1 FROM users LIMIT 1").fetchone()
    return row is not None


def normalize_plate(value: str) -> str:
    return re.sub(r"[^A-Z0-9]", "", value.upper())


def login_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if "user_id" not in session:
            return redirect(url_for("login", next=request.path))
        return view(*args, **kwargs)

    return wrapped


def record_audit(action: str, entity_type: str | None = None, entity_id: int | None = None, details: str | None = None) -> None:
    get_db().execute(
        """
        INSERT INTO audit_log (user_id, action, entity_type, entity_id, details)
        VALUES (?, ?, ?, ?, ?)
        """,
        (session.get("user_id"), action, entity_type, entity_id, details),
    )


def csrf_token() -> str:
    if "csrf_token" not in session:
        session["csrf_token"] = secrets.token_urlsafe(32)
    return session["csrf_token"]


@app.context_processor
def template_context() -> dict[str, Any]:
    return {
        "csrf_token": csrf_token,
        "current_user": session.get("username"),
    }


@app.before_request
def protect_forms() -> None:
    if request.method == "POST":
        supplied = request.form.get("csrf_token", "")
        expected = session.get("csrf_token", "")
        if not expected or not secrets.compare_digest(supplied, expected):
            abort(400, "Invalid form token. Refresh the page and try again.")


@app.route("/setup", methods=["GET", "POST"])
def setup():
    if admin_exists():
        return redirect(url_for("login"))
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        confirmation = request.form.get("confirmation", "")
        if len(username) < 3:
            flash("Username must contain at least three characters.", "error")
        elif len(password) < 10:
            flash("Password must contain at least ten characters.", "error")
        elif password != confirmation:
            flash("The passwords do not match.", "error")
        else:
            connection = get_db()
            cursor = connection.execute(
                """
                INSERT INTO users (username, password_hash, role)
                VALUES (?, ?, 'administrator')
                """,
                (username, generate_password_hash(password)),
            )
            connection.execute(
                """
                INSERT INTO audit_log (user_id, action, entity_type, entity_id, details)
                VALUES (?, 'create_admin', 'user', ?, 'Initial administrator created')
                """,
                (cursor.lastrowid, cursor.lastrowid),
            )
            connection.commit()
            flash("Administrator account created. You can now sign in.", "success")
            return redirect(url_for("login"))
    return render_template("setup.html")


@app.route("/login", methods=["GET", "POST"])
def login():
    if not admin_exists():
        return redirect(url_for("setup"))
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        user = get_db().execute(
            """
            SELECT id, username, password_hash, role
            FROM users
            WHERE username = ? AND is_active = 1
            """,
            (username,),
        ).fetchone()
        if user is None or not check_password_hash(user["password_hash"], password):
            flash("Incorrect username or password.", "error")
        else:
            session.clear()
            session.permanent = True
            session["user_id"] = user["id"]
            session["username"] = user["username"]
            session["role"] = user["role"]
            session["csrf_token"] = secrets.token_urlsafe(32)
            connection = get_db()
            connection.execute(
                "UPDATE users SET last_login_at = CURRENT_TIMESTAMP WHERE id = ?",
                (user["id"],),
            )
            connection.execute(
                "INSERT INTO audit_log (user_id, action, details) VALUES (?, 'login', 'Administrator signed in')",
                (user["id"],),
            )
            connection.commit()
            return redirect(url_for("dashboard"))
    return render_template("login.html")


@app.post("/logout")
@login_required
def logout():
    user_id = session.get("user_id")
    connection = get_db()
    connection.execute(
        "INSERT INTO audit_log (user_id, action, details) VALUES (?, 'logout', 'Administrator signed out')",
        (user_id,),
    )
    connection.commit()
    session.clear()
    return redirect(url_for("login"))


@app.route("/")
@login_required
def dashboard():
    connection = get_db()
    summary = connection.execute(
        """
        SELECT
            (SELECT count(*) FROM vehicles WHERE is_active = 1) AS active_vehicles,
            (SELECT count(*) FROM access_events WHERE date(detected_at, 'localtime') = date('now', 'localtime')) AS events_today,
            (SELECT count(*) FROM access_events WHERE decision = 'authorized' AND date(detected_at, 'localtime') = date('now', 'localtime')) AS authorized_today,
            (SELECT count(*) FROM access_events WHERE decision = 'denied' AND date(detected_at, 'localtime') = date('now', 'localtime')) AS denied_today
        """
    ).fetchone()
    recent_events = connection.execute(
        """
        SELECT e.id, e.plate_number, e.decision, e.gate_action,
               datetime(e.detected_at, 'localtime') AS local_time,
               v.owner_name, v.vehicle_type, v.make, v.model
        FROM access_events e
        LEFT JOIN vehicles v ON v.id = e.vehicle_id
        ORDER BY e.detected_at DESC
        LIMIT 10
        """
    ).fetchall()
    daily = connection.execute(
        """
        SELECT event_date, total_events, authorized_count, denied_count, gates_opened
        FROM daily_access_summary
        ORDER BY event_date DESC
        LIMIT 7
        """
    ).fetchall()
    system = connection.execute("SELECT * FROM system_status WHERE id = 1").fetchone()
    return render_template(
        "dashboard.html",
        summary=summary,
        recent_events=recent_events,
        daily=daily,
        system=system,
    )


@app.route("/vehicles")
@login_required
def vehicles():
    query = request.args.get("q", "").strip()
    if query:
        wildcard = f"%{query}%"
        records = get_db().execute(
            """
            SELECT * FROM vehicles
            WHERE plate_number LIKE ? OR owner_name LIKE ? OR make LIKE ? OR model LIKE ?
            ORDER BY is_active DESC, plate_number
            """,
            (wildcard, wildcard, wildcard, wildcard),
        ).fetchall()
    else:
        records = get_db().execute(
            "SELECT * FROM vehicles ORDER BY is_active DESC, plate_number"
        ).fetchall()
    return render_template("vehicles.html", vehicles=records, query=query)


def vehicle_form_values() -> dict[str, str]:
    fields = (
        "owner_name",
        "vehicle_type",
        "make",
        "model",
        "color",
        "contact_number",
        "email",
        "registration_expires_on",
        "photo_path",
        "notes",
    )
    values = {field: request.form.get(field, "").strip() for field in fields}
    values["plate_number"] = normalize_plate(request.form.get("plate_number", ""))
    return values


@app.route("/vehicles/new", methods=["GET", "POST"])
@login_required
def vehicle_new():
    values: dict[str, Any] = {}
    if request.method == "POST":
        values = vehicle_form_values()
        if not values["plate_number"] or not values["owner_name"]:
            flash("Plate number and owner name are required.", "error")
        else:
            try:
                connection = get_db()
                cursor = connection.execute(
                    """
                    INSERT INTO vehicles (
                        plate_number, owner_name, vehicle_type, make, model, color,
                        contact_number, email, registration_expires_on, photo_path, notes
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    tuple(values[key] or None for key in (
                        "plate_number", "owner_name", "vehicle_type", "make", "model", "color",
                        "contact_number", "email", "registration_expires_on", "photo_path", "notes"
                    )),
                )
                record_audit("create_vehicle", "vehicle", cursor.lastrowid, values["plate_number"])
                connection.commit()
                flash(f'{values["plate_number"]} was registered successfully.', "success")
                return redirect(url_for("vehicles"))
            except sqlite3.IntegrityError:
                flash("That plate number is already registered or is invalid.", "error")
    return render_template("vehicle_form.html", vehicle=values, editing=False)


@app.route("/vehicles/<int:vehicle_id>/edit", methods=["GET", "POST"])
@login_required
def vehicle_edit(vehicle_id: int):
    connection = get_db()
    existing = connection.execute("SELECT * FROM vehicles WHERE id = ?", (vehicle_id,)).fetchone()
    if existing is None:
        abort(404)
    values: dict[str, Any] = dict(existing)
    if request.method == "POST":
        values = vehicle_form_values()
        try:
            connection.execute(
                """
                UPDATE vehicles SET
                    plate_number = ?, owner_name = ?, vehicle_type = ?, make = ?,
                    model = ?, color = ?, contact_number = ?, email = ?,
                    registration_expires_on = ?, photo_path = ?, notes = ?
                WHERE id = ?
                """,
                tuple(values[key] or None for key in (
                    "plate_number", "owner_name", "vehicle_type", "make", "model", "color",
                    "contact_number", "email", "registration_expires_on", "photo_path", "notes"
                )) + (vehicle_id,),
            )
            record_audit("update_vehicle", "vehicle", vehicle_id, values["plate_number"])
            connection.commit()
            flash("Vehicle details updated.", "success")
            return redirect(url_for("vehicles"))
        except sqlite3.IntegrityError:
            flash("That plate number is already registered or is invalid.", "error")
    return render_template("vehicle_form.html", vehicle=values, editing=True)


@app.post("/vehicles/<int:vehicle_id>/toggle")
@login_required
def vehicle_toggle(vehicle_id: int):
    connection = get_db()
    vehicle = connection.execute(
        "SELECT plate_number, is_active FROM vehicles WHERE id = ?", (vehicle_id,)
    ).fetchone()
    if vehicle is None:
        abort(404)
    new_state = 0 if vehicle["is_active"] else 1
    connection.execute("UPDATE vehicles SET is_active = ? WHERE id = ?", (new_state, vehicle_id))
    record_audit(
        "activate_vehicle" if new_state else "deactivate_vehicle",
        "vehicle",
        vehicle_id,
        vehicle["plate_number"],
    )
    connection.commit()
    flash(f'{vehicle["plate_number"]} is now {"active" if new_state else "inactive"}.', "success")
    return redirect(url_for("vehicles"))


def log_filters() -> tuple[list[str], list[Any]]:
    clauses: list[str] = []
    parameters: list[Any] = []
    plate = normalize_plate(request.args.get("plate", ""))
    decision = request.args.get("decision", "").strip()
    date_from = request.args.get("date_from", "").strip()
    date_to = request.args.get("date_to", "").strip()
    if plate:
        clauses.append("e.plate_number LIKE ?")
        parameters.append(f"%{plate}%")
    if decision in {"authorized", "denied", "unreadable", "manual"}:
        clauses.append("e.decision = ?")
        parameters.append(decision)
    if date_from:
        clauses.append("date(e.detected_at, 'localtime') >= date(?)")
        parameters.append(date_from)
    if date_to:
        clauses.append("date(e.detected_at, 'localtime') <= date(?)")
        parameters.append(date_to)
    return clauses, parameters


@app.route("/logs")
@login_required
def logs():
    try:
        page = max(1, int(request.args.get("page", "1")))
    except ValueError:
        page = 1
    per_page = 50
    clauses, parameters = log_filters()
    where = f"WHERE {' AND '.join(clauses)}" if clauses else ""
    connection = get_db()
    total = connection.execute(
        f"SELECT count(*) FROM access_events e {where}", parameters
    ).fetchone()[0]
    records = connection.execute(
        f"""
        SELECT e.*, datetime(e.detected_at, 'localtime') AS local_time,
               v.owner_name, v.vehicle_type, v.make, v.model, v.color
        FROM access_events e
        LEFT JOIN vehicles v ON v.id = e.vehicle_id
        {where}
        ORDER BY e.detected_at DESC
        LIMIT ? OFFSET ?
        """,
        parameters + [per_page, (page - 1) * per_page],
    ).fetchall()
    return render_template(
        "logs.html",
        events=records,
        page=page,
        pages=max(1, math.ceil(total / per_page)),
        total=total,
        filters=request.args,
    )


@app.route("/logs/export.csv")
@login_required
def logs_export():
    clauses, parameters = log_filters()
    where = f"WHERE {' AND '.join(clauses)}" if clauses else ""
    records = get_db().execute(
        f"""
        SELECT datetime(e.detected_at, 'localtime') AS local_time,
               e.plate_number, coalesce(v.owner_name, 'Unknown') AS owner_name,
               coalesce(v.vehicle_type, '') AS vehicle_type,
               coalesce(v.make, '') AS make, coalesce(v.model, '') AS model,
               e.direction, e.decision, e.gate_action,
               e.detector_confidence, e.ocr_confidence, coalesce(e.image_path, '') AS image_path
        FROM access_events e
        LEFT JOIN vehicles v ON v.id = e.vehicle_id
        {where}
        ORDER BY e.detected_at DESC
        """,
        parameters,
    ).fetchall()
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow([
        "Timestamp", "Plate", "Owner", "Vehicle type", "Make", "Model",
        "Direction", "Decision", "Gate action", "Detector confidence",
        "OCR confidence", "Image path",
    ])
    writer.writerows(tuple(row) for row in records)
    return Response(
        output.getvalue(),
        mimetype="text/csv",
        headers={"Content-Disposition": "attachment; filename=access-log.csv"},
    )


@app.route("/events/<int:event_id>/image")
@login_required
def event_image(event_id: int):
    event = get_db().execute(
        "SELECT image_path FROM access_events WHERE id = ?", (event_id,)
    ).fetchone()
    if event is None or not event["image_path"]:
        abort(404)
    image_path = Path(event["image_path"])
    if not image_path.is_absolute():
        image_path = PROJECT_DIR / image_path
    image_path = image_path.resolve()
    try:
        image_path.relative_to(PROJECT_DIR.resolve())
    except ValueError:
        abort(403)
    if not image_path.is_file():
        abort(404)
    return send_file(image_path)


@app.route("/health")
def health():
    get_db().execute("SELECT 1").fetchone()
    return {"status": "ok"}


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", "8080")), debug=False)
