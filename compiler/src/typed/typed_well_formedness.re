open Grain_parsing;
open Types;
open Typedtree;
open TypedtreeIter;

/*
 * This module provides well-formedness checks which are type/value resolution-dependent.
 */

type error =
  | WasmOutsideDisableGc;
exception Error(Location.t, error);

let wasm_unsafe_types = [
  Builtin_types.path_wasmi32,
  Builtin_types.path_wasmi64,
  Builtin_types.path_wasmf32,
  Builtin_types.path_wasmf64,
];

let rec exp_is_wasm_unsafe = ({exp_type}) => {
  let rec type_is_wasm_unsafe = t => {
    switch (t.desc) {
    | TTyConstr(path, _, _) => List.mem(path, wasm_unsafe_types)
    | TTyLink(t) => type_is_wasm_unsafe(t)
    | _ => false
    };
  };
  type_is_wasm_unsafe(exp_type);
};

let is_marked_unsafe = attrs => {
  // Disable_gc implies Unsafe
  List.exists(
    fun
    | {txt: Disable_gc}
    | {txt: Unsafe} => true
    | _ => false,
    attrs,
  );
};

let make_bool_stack = () => {
  let stack = ref([]);
  let push = x => stack := [x, ...stack^];
  let pop = () => {
    switch (stack^) {
    | [] => failwith("Impossible: make_bool_stack > pop")
    | [hd, ...tl] => stack := tl
    };
  };
  let pred = () => {
    switch (stack^) {
    | [] => false
    | [hd, ..._] => hd
    };
  };
  let reset = () => stack := [];
  // For debugging:
  let dump = () =>
    "["
    ++ String.concat(
         ", ",
         List.map(x => if (x) {"true"} else {"false"}, stack^),
       )
    ++ "]";
  (push, pop, pred, reset, dump);
};

let (
  push_in_lambda,
  pop_in_lambda,
  is_in_lambda,
  reset_in_lambda,
  dump_in_lambda,
) =
  make_bool_stack();
let (push_unsafe, pop_unsafe, is_unsafe, reset_unsafe, dump_unsafe) =
  make_bool_stack();

module WellFormednessArg: TypedtreeIter.IteratorArgument = {
  include TypedtreeIter.DefaultIteratorArgument;

  let enter_expression: expression => unit =
    ({exp_desc, exp_loc, exp_attributes} as exp) => {
      // Check: Avoid using Pervasives equality ops with WasmXX types
      switch (exp_desc) {
      | TExpLet(_) when is_marked_unsafe(exp_attributes) => push_unsafe(true)
      | TExpApp(
          {
            exp_desc:
              TExpIdent(
                Path.PExternal(Path.PIdent({name: "Pervasives"}), func, _),
                _,
                _,
              ),
          },
          args,
        )
          when func == "==" || func == "!=" =>
        if (List.exists(exp_is_wasm_unsafe, args)) {
          let warning =
            Grain_utils.Warnings.FuncWasmUnsafe(
              Printf.sprintf("Pervasives.(%s)", func),
            );
          if (Grain_utils.Warnings.is_active(warning)) {
            Grain_parsing.Location.prerr_warning(exp_loc, warning);
          };
        }
      // Check: Warn if using Int32.fromNumber(<literal>)
      | TExpApp(
          {
            exp_desc:
              TExpIdent(
                Path.PExternal(
                  Path.PIdent({name: modname}),
                  "fromNumber",
                  _,
                ),
                _,
                _,
              ),
          },
          [
            {
              exp_desc:
                TExpConstant(
                  Const_number(
                    (Const_number_int(_) | Const_number_float(_)) as n,
                  ),
                ),
            },
          ],
        )
          when
            modname == "Int32"
            || modname == "Int64"
            || modname == "Uint32"
            || modname == "Uint64"
            || modname == "Float32"
            || modname == "Float64" =>
        // NOTE: Due to type-checking, we shouldn't need to worry about ending up with a FloatXX value and a Const_number_float
        let n_str =
          switch (n) {
          | Const_number_int(nint) => Int64.to_string(nint)
          | Const_number_float(nfloat) => Float.to_string(nfloat)
          | _ => failwith("Impossible")
          };
        let warning =
          switch (modname) {
          | "Int32" => Grain_utils.Warnings.FromNumberLiteralI32(n_str)
          | "Int64" => Grain_utils.Warnings.FromNumberLiteralI64(n_str)
          | "Uint32" => Grain_utils.Warnings.FromNumberLiteralU32(n_str)
          | "Uint64" => Grain_utils.Warnings.FromNumberLiteralU64(n_str)
          | "Float32" => Grain_utils.Warnings.FromNumberLiteralF32(n_str)
          | "Float64" => Grain_utils.Warnings.FromNumberLiteralF64(n_str)
          | _ => failwith("Impossible")
          };
        if (Grain_utils.Warnings.is_active(warning)) {
          Grain_parsing.Location.prerr_warning(exp_loc, warning);
        };
      | _ => ()
      };
      // Check: Forbid usage of WasmXX types outside of disableGC context
      switch (exp_desc) {
      | TExpLambda(_) => push_in_lambda(true)
      | _ => ()
      };
      // For now, we only raise the error inside of functions.
      if (exp_is_wasm_unsafe(exp)
          && !(
               Grain_utils.Config.no_gc^
               || Grain_utils.Config.compilation_mode^ == Some("runtime")
               || is_unsafe()
             )) {
        raise(Error(exp_loc, WasmOutsideDisableGc));
      };
    };

  let enter_toplevel_stmt = ({ttop_desc, ttop_attributes}) => {
    switch (ttop_desc) {
    | TTopLet(_) => push_unsafe(is_marked_unsafe(ttop_attributes))
    | _ => ()
    };
  };

  let leave_expression = ({exp_desc, exp_attributes}) => {
    switch (exp_desc) {
    | TExpLet(_) when is_marked_unsafe(exp_attributes) => pop_unsafe()
    | TExpLambda(_) => pop_in_lambda()
    | _ => ()
    };
  };

  let leave_toplevel_stmt = ({ttop_desc}) => {
    switch (ttop_desc) {
    | TTopLet(_) => pop_unsafe()
    | _ => ()
    };
  };
};

module WellFormednessIterator = TypedtreeIter.MakeIterator(WellFormednessArg);

let check_well_formedness = ({statements}) => {
  List.iter(WellFormednessIterator.iter_toplevel_stmt, statements);
};

open Format;

let report_error = ppf =>
  fun
  | WasmOutsideDisableGc =>
    fprintf(
      ppf,
      "Wasm types cannot be used outside of an @unsafe or @disableGC context@.",
    );

let () =
  Location.register_error_of_exn(
    fun
    | Error(loc, err) =>
      Some(Location.error_of_printer(loc, report_error, err))
    | _ => None,
  );
