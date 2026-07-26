import 'dart:io';

import 'package:dcb_gen_tool/src/commands.dart';

Future<void> main(List<String> arguments) async {
  final exitCode = await runCli(arguments);
  exit(exitCode);
}
