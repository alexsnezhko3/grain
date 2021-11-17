open Grain;
open Compile;
open Grain_parsing;
open Grain_utils;
open Grain_typed;

type node_t =
  | Expression(Typedtree.expression)
  | Pattern(Typedtree.pattern)
  | NotInRange
  | Error(string);

let get_raw_pos_info = (pos: Lexing.position) => (
  pos.pos_fname,
  pos.pos_lnum,
  pos.pos_cnum - pos.pos_bol,
  pos.pos_bol,
);

let loc_to_range = (pos: Location.t): Rpc.range_t => {
  let (_, startline, startchar, _) = get_raw_pos_info(pos.loc_start);
  let (_, endline, endchar) =
    Grain_parsing.Location.get_pos_info(pos.loc_end);

  let range: Rpc.range_t = {
    start_line: startline,
    start_char: startchar,
    end_line: endline,
    end_char: endchar,
  };
  range;
};

let is_point_inside_stmt = (loc1: Grain_parsing.Location.t, line: int) => {
  let (_, raw1l, raw1c, _) = get_raw_pos_info(loc1.loc_start);

  let (_, raw1le, raw1ce, _) = get_raw_pos_info(loc1.loc_end);

  if (line == raw1l || line == raw1le) {
    true;
  } else if (line > raw1l && line < raw1le) {
    true;
  } else {
    false;
  };
};
let is_point_inside_location =
    (log, loc1: Grain_parsing.Location.t, line: int, char: int) => {
  let (_, raw1l, raw1c, _) = get_raw_pos_info(loc1.loc_start);
  let (_, raw1le, raw1ce, _) = get_raw_pos_info(loc1.loc_end);

  let res =
    if (line == raw1l) {
      if (char >= raw1c) {
        if (line == raw1le) {
          if (char <= raw1ce) {
            true;
          } else {
            false;
          };
        } else {
          true;
        };
      } else {
        false;
      };
    } else if (line == raw1le) {
      if (char <= raw1ce) {
        true;
      } else {
        false;
      };
    } else if (line > raw1l && line < raw1le) {
      true;
    } else {
      false;
    };

  res;
};

let lens_sig = (t: Types.type_expr) => {
  let buf = Buffer.create(64);
  let ppf = Format.formatter_of_buffer(buf);
  Printtyp.type_expr(ppf, t);
  Format.pp_print_flush(ppf, ());
  let sigStr = Buffer.contents(buf);
  sigStr;
};

let print_expr = (~t: Types.type_expr) => {
  let buf = Buffer.create(64);
  let ppf = Format.formatter_of_buffer(buf);
  Printtyp.type_expr(ppf, t);
  Format.pp_print_flush(ppf, ());
  let sigStr = Buffer.contents(buf);
  sigStr;
};

let rec getNodeFromPattern = (log, pattern: Typedtree.pattern, line, char) => {
  switch (pattern.pat_desc) {
  | TPatTuple(args) =>
    let pats =
      List.filter(
        (p: Typedtree.pattern) =>
          is_point_inside_location(log, p.pat_loc, line, char),
        args,
      );

    if (List.length(pats) == 1) {
      let p = List.hd(pats);
      Pattern(p);
    } else {
      Error("");
    };
  | TPatConstant(c) => Pattern(pattern)
  | TPatVar(v, _) => Pattern(pattern)
  | _ => Error("Pattern")
  };
};

let rec getNodeFromExpression = (log, expr: Typedtree.expression, line, char) => {
  let node =
    switch (expr.exp_desc) {
    | TExpLet(rec_flag, mut_flag, vbs) =>
      if (List.length(vbs) > 0) {
        let matches =
          List.map(
            (vb: Typedtree.value_binding) =>
              getNodeFromExpression(log, vb.vb_expr, line, char),
            vbs,
          );
        let filtered =
          List.filter(
            m =>
              switch (m) {
              | Error(_) => false
              | _ => true
              },
            matches,
          );
        if (List.length(filtered) == 0) {
          // return the type for the whole statement

          let vb = List.hd(vbs);

          let expr = vb.vb_expr;

          Expression(expr);
        } else if (List.length(filtered) == 1) {
          List.hd(filtered);
        } else {
          Error("Too many let matches");
        };
      } else {
        Error("");
      }
    | TExpBlock(expressions) =>
      log(
        "TExpBlock has  "
        ++ string_of_int(List.length(expressions))
        ++ " expressions",
      );
      if (List.length(expressions) > 0) {
        let exps =
          List.filter(
            (e: Typedtree.expression) =>
              is_point_inside_location(log, e.exp_loc, line, char),
            expressions,
          );

        log(
          "TExpBlock has  "
          ++ string_of_int(List.length(exps))
          ++ " matching expressions",
        );
        if (List.length(exps) == 0) {
          NotInRange//   ("No block matches");
                    ; // Error
                    //Expression(expr);
        } else if (List.length(exps) == 1) {
          let expr = List.hd(exps);
          getNodeFromExpression(log, expr, line, char);
        } else {
          Error(string_of_int(List.length(exps)) ++ " block matches");
        };
      } else {
        Error("");
      };

    | TExpApp(func, expressions) =>
      if (is_point_inside_location(log, func.exp_loc, line, char)) {
        Expression(func);
      } else if (List.length(expressions) > 0) {
        let exps =
          List.filter(
            (e: Typedtree.expression) =>
              is_point_inside_location(log, e.exp_loc, line, char),
            expressions,
          );

        if (List.length(exps) == 0) {
          Error("No fn app matches");
        } else if (List.length(exps) == 1) {
          let expr = List.hd(exps);
          Expression(expr);
        } else {
          Error(
            string_of_int(List.length(exps)) ++ "Multiple fn app matches",
          );
        };
      } else {
        Error("");
      }
    | TExpLambda([{mb_pat: pattern, mb_body: body}], _) =>
      let node = getNodeFromPattern(log, pattern, line, char);
      switch (node) {
      | Error(_) => getNodeFromExpression(log, body, line, char)
      | _ => node
      };
    | TExpIf(cond, trueexp, falseexp) =>
      // let node = getNodeFromExpression(log, cond, line, char);

      let condNode = getNodeFromExpression(log, cond, line, char);

      let trueMatch =
        switch (condNode) {
        | NotInRange
        | Error(_) => getNodeFromExpression(log, trueexp, line, char)
        | _ => condNode
        };

      switch (trueMatch) {
      | NotInRange
      | Error(_) => getNodeFromExpression(log, falseexp, line, char)
      | _ => trueMatch
      };

    | TExpArray(expressions)
    | TExpTuple(expressions) =>
      let exps =
        List.filter(
          (e: Typedtree.expression) =>
            is_point_inside_location(log, e.exp_loc, line, char),
          expressions,
        );

      if (List.length(exps) == 0) {
        Error("No match");
      } else if (List.length(exps) == 1) {
        let expr = List.hd(exps);
        Expression(expr);
      } else {
        Error(string_of_int(List.length(exps)) ++ "Multiple matches");
      };

    | TExpLambda([], _) => failwith("Impossible: transl_imm: Empty lambda")
    | TExpLambda(_, _) => failwith("NYI: transl_imm: Multi-branch lambda")
    | TExpContinue => Error("TExpContinue")
    | TExpBreak => Error("TExpBreak")
    | TExpNull => Error("TExpNull")
    | TExpIdent(_) => Error("TExpIdent")
    | TExpConstant(const) =>
      if (is_point_inside_location(log, expr.exp_loc, line, char)) {
        Expression(expr);
      } else {
        NotInRange;
      }

    | TExpArrayGet(_) => Error("TExpArrayGet")
    | TExpArraySet(_) => Error("TExpArraySet")
    | TExpRecord(_) => Error("TExpRecord")
    | TExpRecordGet(_) => Error("TExpRecordGet")
    | TExpRecordSet(_) => Error("TExpRecordSet")
    | TExpMatch(_) => Error("TExpMatch")
    | TExpPrim1(_) => Error("TExpPrim1")
    | TExpPrim2(_) => Error("TExpPrim2")
    | TExpPrimN(_) => Error("TExpPrimN")
    | TExpBoxAssign(_) => Error("TExpBoxAssign")
    | TExpAssign(_) => Error("TExpAssign")

    | TExpWhile(_) => Error("TExpWhile")
    | TExpFor(_) => Error("TExpFor")
    | TExpConstruct(_) => Error("TExpConstruct")
    };
  node;
};

let getHoverFromStmt = (log, stmt: Typedtree.toplevel_stmt, line, char) =>
  switch (stmt.ttop_desc) {
  | TTopImport(import_declaration) => ("import declaration", None)
  | TTopForeign(export_flag, value_description) => ("foreign", None)
  | TTopData(data_declarations) => ("data declaration", None)
  | TTopLet(export_flag, rec_flag, mut_flag, value_bindings) =>
    log("@@@ TTopLet");
    log("number of vbs is " ++ string_of_int(List.length(value_bindings)));
    if (List.length(value_bindings) > 0) {
      // let vbs: Typedtree.value_binding = List.hd(value_bindings);
      // getHoverFromExpression(log, vbs.vb_expr, line, char);
      let matches =
        List.map(
          (vb: Typedtree.value_binding) =>
            getNodeFromExpression(log, vb.vb_expr, line, char),
          value_bindings,
        );
      let filtered =
        List.filter(
          m =>
            switch (m) {
            | Error(_) => false
            | NotInRange => false
            | _ => true
            },
          matches,
        );
      if (List.length(filtered) == 0) {
        // return the type for the whole statement

        let vb = List.hd(value_bindings);

        let expr = vb.vb_expr;

        // (lens_sig(expr.exp_type), Some(vb.vb_loc));

        ("no match 44", Some(vb.vb_loc));
      } else if (List.length(filtered) == 1) {
        let node = List.hd(filtered);
        switch (node) {
        | Error(err) => (err, None)
        | NotInRange => ("Not in range", None)
        | Expression(e) => (lens_sig(e.exp_type), Some(e.exp_loc))
        | Pattern(p) => (lens_sig(p.pat_type), Some(p.pat_loc))
        };
      } else {
        ("Too many ttoplet matches", None);
      };
    } else {
      ("", None);
    };

  | TTopExpr(expression) =>
    log("@@@ TTopExpr");
    // let expr_type = expression.exp_type;

    // lens_sig(expr_type);
    let node = getNodeFromExpression(log, expression, line, char);
    switch (node) {
    | Error(err) => (err, None)
    | NotInRange => (
        lens_sig(expression.exp_type),
        Some(expression.exp_loc),
      )
    | Expression(e) => (lens_sig(e.exp_type), Some(e.exp_loc))
    | Pattern(p) => (lens_sig(p.pat_type), Some(p.pat_loc))
    };

  | TTopException(export_flag, type_exception) => ("exception", None)
  | TTopExport(export_declarations) => ("export", None)
  };

let findBestMatch = (typed_program: Typedtree.typed_program, line, char) => {
  // see if it falls within a top level statement (which it must!)

  let rec loop = (statements: list(Grain_typed.Typedtree.toplevel_stmt)) =>
    if (List.length(statements) < 1) {
      None;
    } else {
      let stmt = List.hd(statements);
      let loc = stmt.ttop_loc;

      if (is_point_inside_stmt(loc, line)) {
        Some(stmt);
      } else {
        loop(List.tl(statements));
      };
    };
  loop(typed_program.statements);
};

let getNodeFromStmt =
    (log, stmt: Grain_typed__Typedtree.toplevel_stmt, line, char) =>
  switch (stmt.ttop_desc) {
  | TTopImport(import_declaration) => Error("import declaration")
  | TTopForeign(export_flag, value_description) => Error("foreign")
  | TTopData(data_declarations) => Error("data declaration")
  | TTopLet(export_flag, rec_flag, mut_flag, value_bindings) =>
    log("@@@ TTopLet");
    log("number of vbs is " ++ string_of_int(List.length(value_bindings)));
    if (List.length(value_bindings) > 0) {
      // let vbs: Typedtree.value_binding = List.hd(value_bindings);
      // getHoverFromExpression(log, vbs.vb_expr, line, char);
      let matches =
        List.map(
          (vb: Typedtree.value_binding) =>
            getNodeFromExpression(log, vb.vb_expr, line, char),
          value_bindings,
        );
      let filtered =
        List.filter(
          m =>
            switch (m) {
            | Error(_) => false
            | _ => true
            },
          matches,
        );
      if (List.length(filtered) == 0) {
        // return the type for the whole statement

        let vb = List.hd(value_bindings);

        let expr = vb.vb_expr;

        Expression(expr);
        //(lens_sig(expr.exp_type), Some(vb.vb_loc));
        //   ("No TTOPLET match", None);
      } else if (List.length(filtered) == 1) {
        List.hd(filtered);
      } else {
        Error("Too many ttoplet matches");
      };
    } else {
      Error("");
    };

  | TTopExpr(expression) =>
    log("@@@ TTopExpr");
    // let expr_type = expression.exp_type;

    // lens_sig(expr_type);
    getNodeFromExpression(log, expression, line, char);
  | TTopException(export_flag, type_exception) => Error("exception")
  | TTopExport(export_declarations) => Error("export")
  };

let rec print_ident = (ident: Identifier.t) => {
  switch (ident) {
  | IdentName(name) => name

  | IdentExternal(externalIdent, second) =>
    print_ident(externalIdent) ++ "." ++ second
  };
};

let rec print_path = (ident: Path.t) => {
  switch (ident) {
  | PIdent(name) => name.name

  | PExternal(externalIdent, second, _) =>
    print_path(externalIdent) ++ "." ++ second
  };
};

let getTextDocumenUriAndPosition = json => {
  let params = Yojson.Safe.Util.member("params", json);

  let textDocument = Yojson.Safe.Util.member("textDocument", params);

  let position = Yojson.Safe.Util.member("position", params);

  let uri =
    Yojson.Safe.Util.member("uri", textDocument)
    |> Yojson.Safe.Util.to_string_option;

  let line =
    Yojson.Safe.Util.member("line", position)
    |> Yojson.Safe.Util.to_int_option;

  let char =
    Yojson.Safe.Util.member("character", position)
    |> Yojson.Safe.Util.to_int_option;

  (uri, line, char);
};
